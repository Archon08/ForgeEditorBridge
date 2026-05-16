#include "Handlers/MaterialHandler.h"
#include "ForgeAISubsystem.h"
#include "Attention/BridgeAttentionManager.h"
#include "Capture/ForgeMaterialCapture.h"
#include "BridgeSessionStore.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionStaticBoolParameter.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialFunction.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "Factories/MaterialFactoryNew.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Guid.h"
#include "Modules/ModuleManager.h"
#include "Editor.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UObject/Package.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static FString SerializeMat(const TSharedPtr<FJsonObject>& Obj)
{
    FString Out;
    TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Obj.ToSharedRef(), W);
    return Out;
}

static UMaterial* GetTargetMaterial(UBridgeSubsystem* Subsystem, TSharedPtr<FJsonObject> Params, FString& OutError)
{
    FString AssetPath;
    if (Params.IsValid())
    {
        if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
            Params->TryGetStringField(TEXT("path"), AssetPath);
    }
    if (AssetPath.IsEmpty() && Subsystem && Subsystem->AttentionManager)
        AssetPath = Subsystem->AttentionManager->GetMaterialTargetPath();

    if (AssetPath.IsEmpty()) { OutError = TEXT("No asset_path and no material target in AttentionManager"); return nullptr; }

    // Try open editor first (live instance has MaterialGraph)
    if (GEditor)
    {
        if (UAssetEditorSubsystem* AESub = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
        {
            if (UObject* Obj = FSoftObjectPath(AssetPath).ResolveObject())
            {
                if (AESub->FindEditorForAsset(Obj, false))
                    if (UMaterial* Mat = Cast<UMaterial>(Obj)) return Mat;
            }
        }
    }

    UMaterial* Mat = LoadObject<UMaterial>(nullptr, *AssetPath, nullptr, LOAD_NoWarn);
    if (!Mat) { OutError = FString::Printf(TEXT("Could not load material: %s"), *AssetPath); return nullptr; }
    return Mat;
}

static UMaterialExpression* FindExpressionByHandle(UMaterial* Mat, const FString& Handle)
{
    if (!Mat) return nullptr;
    for (UMaterialExpression* Expr : Mat->GetExpressions())
    {
        if (!Expr) continue;
        if (Expr->GetName().Equals(Handle, ESearchCase::IgnoreCase) ||
            Expr->Desc.Equals(Handle, ESearchCase::IgnoreCase))
            return Expr;
    }
    return nullptr;
}

static void ForceRefreshMaterialEditor(UMaterial* Mat)
{
    if (!Mat) return;
    if (Mat->MaterialGraph) Mat->MaterialGraph->NotifyGraphChanged();
    Mat->Modify();
    Mat->PostEditChange();
    Mat->MarkPackageDirty();
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
FBridgeResult UMaterialHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
    {
        FBridgeResult R = CreateResult(TEXT("material"), Action);
        R.Message = TEXT("Params object is null");
        R.ErrorCode = 1000;
        return R;
    }

    FBridgeResult Result = CreateResult(TEXT("material"), Action);

    // ---- material_set_target ------------------------------------------------
    if (Action == TEXT("material_set_target"))
    {
        FString Path;
        if (!Params->TryGetStringField(TEXT("asset_path"), Path) || Path.IsEmpty())
        { Result.Message = TEXT("material_set_target: 'asset_path' required"); Result.ErrorCode = 1000; return Result; }

        // Try to find in open editor
        UMaterial* Mat = nullptr;
        if (GEditor)
        {
            if (UAssetEditorSubsystem* AESub = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
            {
                if (UObject* Obj = FSoftObjectPath(Path).ResolveObject())
                    if (AESub->FindEditorForAsset(Obj, false))
                        Mat = Cast<UMaterial>(Obj);
            }
        }
        if (!Mat) Mat = LoadObject<UMaterial>(nullptr, *Path, nullptr, LOAD_NoWarn);

        // Create if not found and path is valid
        bool bCreate = true;
        if (Params->HasField(TEXT("create_if_missing")))
            bCreate = Params->GetBoolField(TEXT("create_if_missing"));

        if (!Mat && bCreate && FPackageName::IsValidObjectPath(Path))
        {
            FString ShortName = FPackageName::GetShortName(Path);
            UPackage* Package = CreatePackage(*Path);
            UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
            Mat = Cast<UMaterial>(Factory->FactoryCreateNew(
                UMaterial::StaticClass(), Package, *ShortName,
                RF_Public | RF_Standalone, nullptr, GWarn));
            if (Mat)
            {
                Mat->MaterialDomain = MD_UI;
                Mat->BlendMode = BLEND_Translucent;
                FAssetRegistryModule::AssetCreated(Mat);
                Mat->MarkPackageDirty();
            }
        }

        if (!Mat) { Result.Message = FString::Printf(TEXT("material_set_target: Could not find or create: %s"), *Path); Result.ErrorCode = 2000; Result.RecoveryHint = TEXT("Verify the asset path or set create_if_missing=true."); return Result; }

        if (Subsystem && Subsystem->AttentionManager)
            Subsystem->AttentionManager->SetMaterialTargetPath(Path);

        Result.bSuccess     = true;
        Result.AffectedPath = Path;
        Result.Message      = FString::Printf(TEXT("Target material set: %s"), *Path);
        return Result;
    }

    // ---- create_material_function (no target material required) -------------
    if (Action == TEXT("create_material_function"))
        return Action_CreateMaterialFunction(Params);

    // ---- bake_textures (handles its own target resolution) ------------------
    if (Action == TEXT("bake_textures"))
        return Action_BakeTextures(Params);

    // ---- add_parameter (handles its own target resolution) ------------------
    if (Action == TEXT("add_parameter"))
        return Action_AddParameter(Params);

    // Remaining actions need the material
    FString MatError;
    UMaterial* Mat = GetTargetMaterial(Subsystem, Params, MatError);
    if (!Mat) { Result.Message = MatError; Result.ErrorCode = 2000; return Result; }

    // ---- material_add_node --------------------------------------------------
    if (Action == TEXT("material_add_node"))
    {
        FString NodeClass, NodeName;
        if (!Params->TryGetStringField(TEXT("node_type"), NodeClass) || NodeClass.IsEmpty())
        { Result.Message = TEXT("material_add_node: 'node_type' required (e.g. Add, Multiply, TextureSample)"); Result.ErrorCode = 1000; return Result; }
        Params->TryGetStringField(TEXT("node_name"), NodeName);

        // Resolve class: try full path, then /Script/Engine.MaterialExpression<NodeClass>
        UClass* ExprClass = FindObject<UClass>(nullptr, *NodeClass);
        if (!ExprClass)
        {
            FString EngPath = TEXT("/Script/Engine.MaterialExpression") + NodeClass;
            ExprClass = FindObject<UClass>(nullptr, *EngPath);
        }
        if (!ExprClass)
        {
            FString EngPath = TEXT("/Script/Engine.MaterialExpression") + NodeClass;
            ExprClass = LoadObject<UClass>(nullptr, *EngPath);
        }
        if (!ExprClass || !ExprClass->IsChildOf(UMaterialExpression::StaticClass()))
        { Result.Message = FString::Printf(TEXT("material_add_node: Unknown node type '%s'"), *NodeClass); Result.ErrorCode = 1001; return Result; }

        UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpression(Mat, ExprClass);
        if (!NewExpr) { Result.Message = TEXT("material_add_node: CreateMaterialExpression failed"); Result.ErrorCode = 3000; return Result; }

        if (!NodeName.IsEmpty()) NewExpr->Desc = NodeName;

        // Auto-stagger position
        int32 PosX = -400, PosY = 0;
        if (Params->HasField(TEXT("x"))) PosX = (int32)Params->GetNumberField(TEXT("x"));
        if (Params->HasField(TEXT("y"))) PosY = (int32)Params->GetNumberField(TEXT("y"));
        else if (Mat->GetEditorOnlyData())
            PosY = Mat->GetEditorOnlyData()->ExpressionCollection.Expressions.Num() * 100;
        NewExpr->MaterialExpressionEditorX = PosX;
        NewExpr->MaterialExpressionEditorY = PosY;

        Mat->MarkPackageDirty();
        ForceRefreshMaterialEditor(Mat);

        Result.bSuccess     = true;
        Result.AffectedPath = NewExpr->GetName();
        Result.Message      = FString::Printf(TEXT("Added node '%s' (handle: %s)"), *NodeClass, *NewExpr->GetName());
        return Result;
    }

    // ---- material_connect_pins ----------------------------------------------
    if (Action == TEXT("material_connect_pins") || Action == TEXT("material_connect_nodes"))
    {
        FString FromHandle, FromPin, ToHandle, ToPin;
        Params->TryGetStringField(TEXT("from_node"), FromHandle);
        Params->TryGetStringField(TEXT("from_pin"),  FromPin);
        Params->TryGetStringField(TEXT("to_node"),   ToHandle);
        Params->TryGetStringField(TEXT("to_pin"),    ToPin);
        if (Action == TEXT("material_connect_nodes")) { FromPin = TEXT(""); ToPin = TEXT(""); }

        bool bIsRootTarget = ToHandle.StartsWith(TEXT("Master")) ||
            ToHandle.Equals(TEXT("Output"), ESearchCase::IgnoreCase) ||
            ToHandle.Equals(TEXT("Root"), ESearchCase::IgnoreCase) ||
            ToHandle.Equals(TEXT("Material"), ESearchCase::IgnoreCase) ||
            ToHandle.Equals(TEXT("MaterialRoot"), ESearchCase::IgnoreCase) ||
            ToHandle.Equals(TEXT("MaterialResult"), ESearchCase::IgnoreCase) ||
            ToHandle.Equals(Mat->GetName(), ESearchCase::IgnoreCase);

        if (bIsRootTarget && Mat->MaterialGraph)
        {
            // Graph-based root connection
            UMaterialGraphNode* SrcGraphNode = nullptr;
            for (UEdGraphNode* N : Mat->MaterialGraph->Nodes)
            {
                UMaterialGraphNode* MN = Cast<UMaterialGraphNode>(N);
                if (MN && MN->MaterialExpression &&
                    (MN->MaterialExpression->GetName().Equals(FromHandle, ESearchCase::IgnoreCase) ||
                     MN->MaterialExpression->Desc.Equals(FromHandle, ESearchCase::IgnoreCase)))
                { SrcGraphNode = MN; break; }
            }

            UEdGraphNode* RootNode = nullptr;
            for (UEdGraphNode* N : Mat->MaterialGraph->Nodes)
                if (N->IsA(UMaterialGraphNode_Root::StaticClass())) { RootNode = N; break; }

            if (!SrcGraphNode || !RootNode)
            {
                Result.Message = FString::Printf(TEXT("material_connect_pins: source '%s' or root node not found in MaterialGraph"), *FromHandle);
                Result.ErrorCode = 2000;
                return Result;
            }

            // Find output pin on source
            UEdGraphPin* SrcPin = nullptr;
            for (UEdGraphPin* Pin : SrcGraphNode->Pins)
            {
                if (Pin->Direction != EGPD_Output) continue;
                if (FromPin.IsEmpty() || Pin->PinName.ToString().Equals(FromPin, ESearchCase::IgnoreCase))
                { SrcPin = Pin; break; }
            }

            // Map root pin name aliases
            FString TargetPinName = ToPin;
            if (TargetPinName.IsEmpty() || TargetPinName.Equals(TEXT("Output"), ESearchCase::IgnoreCase))
                TargetPinName = (Mat->MaterialDomain == MD_UI) ? TEXT("EmissiveColor") : TEXT("BaseColor");

            UEdGraphPin* DstPin = nullptr;
            for (UEdGraphPin* Pin : RootNode->Pins)
            {
                if (Pin->Direction != EGPD_Input) continue;
                if (Pin->PinName.ToString().Equals(TargetPinName, ESearchCase::IgnoreCase))
                { DstPin = Pin; break; }
            }
            // Fallback: partial match
            if (!DstPin)
            {
                for (UEdGraphPin* Pin : RootNode->Pins)
                    if (Pin->Direction == EGPD_Input && Pin->PinName.ToString().Contains(TargetPinName))
                    { DstPin = Pin; break; }
            }

            if (!SrcPin || !DstPin)
            {
                Result.Message = FString::Printf(TEXT("material_connect_pins: pin not found — src=%s, dst=%s"),
                    SrcPin ? TEXT("ok") : TEXT("MISSING"),
                    DstPin ? TEXT("ok") : TEXT("MISSING"));
                Result.ErrorCode = 2000;
                return Result;
            }

            SrcPin->MakeLinkTo(DstPin);
            ForceRefreshMaterialEditor(Mat);
            Result.bSuccess = true;
            Result.Message  = FString::Printf(TEXT("Connected '%s' -> Root.%s"), *FromHandle, *TargetPinName);
            return Result;
        }

        // Root connection fallback when MaterialGraph is unavailable
        if (bIsRootTarget && !Mat->MaterialGraph)
        {
            UMaterialExpression* FromExpr = FindExpressionByHandle(Mat, FromHandle);
            if (!FromExpr) { Result.Message = FString::Printf(TEXT("material_connect_pins: source '%s' not found"), *FromHandle); Result.ErrorCode = 2000; return Result; }

            // Map pin name to material property
            FString TargetPinName = ToPin;
            if (TargetPinName.IsEmpty() || TargetPinName.Equals(TEXT("Output"), ESearchCase::IgnoreCase))
                TargetPinName = (Mat->MaterialDomain == MD_UI) ? TEXT("EmissiveColor") : TEXT("BaseColor");

            bool bConnected = false;
            // Match pin name to EMaterialProperty
            if (TargetPinName.Equals(TEXT("BaseColor"), ESearchCase::IgnoreCase))
                bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(FromExpr, TEXT(""), EMaterialProperty::MP_BaseColor);
            else if (TargetPinName.Equals(TEXT("Metallic"), ESearchCase::IgnoreCase))
                bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(FromExpr, TEXT(""), EMaterialProperty::MP_Metallic);
            else if (TargetPinName.Equals(TEXT("Roughness"), ESearchCase::IgnoreCase))
                bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(FromExpr, TEXT(""), EMaterialProperty::MP_Roughness);
            else if (TargetPinName.Equals(TEXT("EmissiveColor"), ESearchCase::IgnoreCase))
                bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(FromExpr, TEXT(""), EMaterialProperty::MP_EmissiveColor);
            else if (TargetPinName.Equals(TEXT("Normal"), ESearchCase::IgnoreCase))
                bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(FromExpr, TEXT(""), EMaterialProperty::MP_Normal);
            else if (TargetPinName.Equals(TEXT("Opacity"), ESearchCase::IgnoreCase))
                bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(FromExpr, TEXT(""), EMaterialProperty::MP_Opacity);

            if (bConnected) { Mat->MarkPackageDirty(); Result.bSuccess = true; Result.Message = FString::Printf(TEXT("Connected '%s' -> Root.%s (via EditingLibrary)"), *FromHandle, *TargetPinName); }
            else { Result.Message = FString::Printf(TEXT("material_connect_pins: ConnectMaterialProperty failed for '%s' -> %s"), *FromHandle, *TargetPinName); Result.ErrorCode = 3000; }
            return Result;
        }

        // Non-root: reflection-based connection
        UMaterialExpression* FromExpr = FindExpressionByHandle(Mat, FromHandle);
        UMaterialExpression* ToExpr   = FindExpressionByHandle(Mat, ToHandle);
        if (!FromExpr) { Result.Message = FString::Printf(TEXT("material_connect_pins: source '%s' not found"), *FromHandle); Result.ErrorCode = 2000; return Result; }
        if (!ToExpr)   { Result.Message = FString::Printf(TEXT("material_connect_pins: target '%s' not found"), *ToHandle); Result.ErrorCode = 2000; return Result; }

        // Find input property by reflection
        FExpressionInput* InputPtr = nullptr;
        TArray<FString> TryPins = ToPin.IsEmpty()
            ? TArray<FString>{ TEXT("Input"), TEXT("A"), TEXT("Coordinates"), TEXT("UV") }
            : TArray<FString>{ ToPin };

        for (UObject* Target : TArray<UObject*>{ ToExpr })
        {
            for (TFieldIterator<FProperty> PropIt(Target->GetClass()); PropIt; ++PropIt)
            {
                FStructProperty* SP = CastField<FStructProperty>(*PropIt);
                if (!SP || !SP->Struct) continue;
                FString SName = SP->Struct->GetName();
                if (!SName.Contains(TEXT("ExpressionInput")) && !SName.Contains(TEXT("MaterialInput"))) continue;
                for (const FString& TryPin : TryPins)
                {
                    if (SP->GetName().Equals(TryPin, ESearchCase::IgnoreCase))
                    {
                        InputPtr = SP->ContainerPtrToValuePtr<FExpressionInput>(Target);
                        break;
                    }
                }
                if (!TryPins[0].IsEmpty() && !InputPtr)
                {
                    // Try first matching ExpressionInput
                    if (InputPtr == nullptr)
                        InputPtr = SP->ContainerPtrToValuePtr<FExpressionInput>(Target);
                }
                if (InputPtr) break;
            }
            if (InputPtr) break;
        }

        // Special: UMaterialExpressionCustom named inputs
        if (!InputPtr)
        {
            if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(ToExpr))
            {
                for (int32 i = 0; i < Custom->Inputs.Num(); ++i)
                    if (Custom->Inputs[i].InputName.ToString().Equals(ToPin, ESearchCase::IgnoreCase))
                    { InputPtr = &Custom->Inputs[i].Input; break; }
            }
        }

        if (!InputPtr) { Result.Message = FString::Printf(TEXT("material_connect_pins: input pin '%s' not found on '%s'"), *ToPin, *ToHandle); Result.ErrorCode = 2000; return Result; }

        InputPtr->Expression = FromExpr;
        InputPtr->OutputIndex = 0;
        ForceRefreshMaterialEditor(Mat);

        Result.bSuccess = true;
        Result.Message  = FString::Printf(TEXT("Connected '%s' -> '%s'.%s"), *FromHandle, *ToHandle, *ToPin);
        return Result;
    }

    // ---- material_get_graph -------------------------------------------------
    if (Action == TEXT("material_get_graph"))
    {
        TArray<TSharedPtr<FJsonValue>> NodeArr;
        if (Mat->MaterialGraph)
        {
            for (UEdGraphNode* N : Mat->MaterialGraph->Nodes)
            {
                UMaterialGraphNode* MN = Cast<UMaterialGraphNode>(N);
                TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
                if (MN && MN->MaterialExpression)
                {
                    NObj->SetStringField(TEXT("handle"), MN->MaterialExpression->GetName());
                    NObj->SetStringField(TEXT("desc"),   MN->MaterialExpression->Desc);
                    NObj->SetStringField(TEXT("class"),  MN->MaterialExpression->GetClass()->GetName());
                }
                else
                {
                    NObj->SetStringField(TEXT("handle"), N->GetName());
                    NObj->SetStringField(TEXT("class"),  N->GetClass()->GetName());
                }
                NObj->SetNumberField(TEXT("x"), N->NodePosX);
                NObj->SetNumberField(TEXT("y"), N->NodePosY);
                NodeArr.Add(MakeShared<FJsonValueObject>(NObj));
            }
        }
        else
        {
            // No MaterialGraph (not open in editor) — iterate expressions
            for (UMaterialExpression* Expr : Mat->GetExpressions())
            {
                if (!Expr) continue;
                TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
                NObj->SetStringField(TEXT("handle"), Expr->GetName());
                NObj->SetStringField(TEXT("desc"),   Expr->Desc);
                NObj->SetStringField(TEXT("class"),  Expr->GetClass()->GetName());
                NObj->SetNumberField(TEXT("x"), Expr->MaterialExpressionEditorX);
                NObj->SetNumberField(TEXT("y"), Expr->MaterialExpressionEditorY);
                NodeArr.Add(MakeShared<FJsonValueObject>(NObj));
            }
        }

        TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
        DataObj->SetArrayField(TEXT("nodes"), NodeArr);
        DataObj->SetStringField(TEXT("material"), Mat->GetName());

        Result.bSuccess  = true;
        Result.ExtraData = SerializeMat(DataObj);
        Result.Message   = FString::Printf(TEXT("Returned %d nodes from material graph"), NodeArr.Num());
        return Result;
    }

    // ---- material_get_pins --------------------------------------------------
    if (Action == TEXT("material_get_pins"))
    {
        FString NodeHandle;
        if (!Params->TryGetStringField(TEXT("node_id"), NodeHandle) || NodeHandle.IsEmpty())
            Params->TryGetStringField(TEXT("handle"), NodeHandle);
        if (NodeHandle.IsEmpty())
        { Result.Message = TEXT("material_get_pins: 'node_id' (or 'handle') required"); Result.ErrorCode = 1000; return Result; }

        UMaterialExpression* Expr = FindExpressionByHandle(Mat, NodeHandle);
        if (!Expr) { Result.Message = FString::Printf(TEXT("material_get_pins: node '%s' not found"), *NodeHandle); Result.ErrorCode = 2000; return Result; }

        TArray<TSharedPtr<FJsonValue>> InputArr, OutputArr;
        for (TFieldIterator<FProperty> PropIt(Expr->GetClass()); PropIt; ++PropIt)
        {
            FStructProperty* SP = CastField<FStructProperty>(*PropIt);
            if (!SP || !SP->Struct) continue;
            FString SName = SP->Struct->GetName();
            if (SName.Contains(TEXT("ExpressionInput")) || SName.Contains(TEXT("MaterialInput")))
            {
                TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
                PObj->SetStringField(TEXT("name"), SP->GetName());
                InputArr.Add(MakeShared<FJsonValueObject>(PObj));
            }
        }
        // Outputs from MaterialGraph node if available
        bool bOutputsFound = false;
        if (Mat->MaterialGraph)
        {
            for (UEdGraphNode* N : Mat->MaterialGraph->Nodes)
            {
                UMaterialGraphNode* MN = Cast<UMaterialGraphNode>(N);
                if (MN && MN->MaterialExpression == Expr)
                {
                    for (UEdGraphPin* Pin : MN->Pins)
                        if (Pin->Direction == EGPD_Output)
                        {
                            TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
                            PObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
                            OutputArr.Add(MakeShared<FJsonValueObject>(PObj));
                        }
                    bOutputsFound = true;
                    break;
                }
            }
        }
        // Fallback: enumerate outputs via expression reflection when MaterialGraph is unavailable
        if (!bOutputsFound)
        {
            TArray<FExpressionOutput>& Outputs = Expr->GetOutputs();
            if (Outputs.Num() > 0)
            {
                for (int32 i = 0; i < Outputs.Num(); ++i)
                {
                    TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
                    FString OutName = Outputs[i].OutputName.IsNone()
                        ? FString::Printf(TEXT("Output_%d"), i)
                        : Outputs[i].OutputName.ToString();
                    PObj->SetStringField(TEXT("name"), OutName);
                    PObj->SetNumberField(TEXT("index"), (double)i);
                    OutputArr.Add(MakeShared<FJsonValueObject>(PObj));
                }
            }
            else
            {
                // Most expressions have at least one implicit output
                TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
                PObj->SetStringField(TEXT("name"), TEXT("Output"));
                PObj->SetNumberField(TEXT("index"), 0.0);
                OutputArr.Add(MakeShared<FJsonValueObject>(PObj));
            }
        }

        TSharedPtr<FJsonObject> DataObj = MakeShared<FJsonObject>();
        DataObj->SetArrayField(TEXT("inputs"),  InputArr);
        DataObj->SetArrayField(TEXT("outputs"), OutputArr);

        Result.bSuccess  = true;
        Result.ExtraData = SerializeMat(DataObj);
        Result.Message   = FString::Printf(TEXT("Pins for node '%s': %d inputs, %d outputs"),
            *NodeHandle, InputArr.Num(), OutputArr.Num());
        return Result;
    }

    // ---- material_set_node_properties ---------------------------------------
    if (Action == TEXT("material_set_node_properties"))
    {
        FString NodeHandle;
        if (!Params->TryGetStringField(TEXT("node_id"), NodeHandle) || NodeHandle.IsEmpty())
            Params->TryGetStringField(TEXT("handle"), NodeHandle);
        if (NodeHandle.IsEmpty())
        { Result.Message = TEXT("material_set_node_properties: 'node_id' (or 'handle') required"); Result.ErrorCode = 1000; return Result; }

        const TSharedPtr<FJsonObject>* PropsPtr;
        if (!Params->TryGetObjectField(TEXT("properties"), PropsPtr))
        { Result.Message = TEXT("material_set_node_properties: 'properties' object required"); Result.ErrorCode = 1000; return Result; }

        UMaterialExpression* Expr = FindExpressionByHandle(Mat, NodeHandle);
        if (!Expr) { Result.Message = FString::Printf(TEXT("material_set_node_properties: node '%s' not found"), *NodeHandle); Result.ErrorCode = 2000; return Result; }

        int32 Applied = 0;
        for (auto& Pair : (*PropsPtr)->Values)
        {
            FProperty* Prop = Expr->GetClass()->FindPropertyByName(FName(*Pair.Key));
            if (!Prop) continue;

            void* Addr = Prop->ContainerPtrToValuePtr<void>(Expr);
            FString ValStr;

            if (Pair.Value->TryGetString(ValStr))
            {
                // Direct string — use ImportText
                Prop->ImportText_Direct(*ValStr, Addr, Expr, PPF_None);
                ++Applied;
            }
            else if (Pair.Value->Type == EJson::Number)
            {
                // Numeric value — try float/double/int
                double NumVal = Pair.Value->AsNumber();
                if (FFloatProperty* FP = CastField<FFloatProperty>(Prop)) { FP->SetPropertyValue(Addr, (float)NumVal); ++Applied; }
                else if (FDoubleProperty* DP = CastField<FDoubleProperty>(Prop)) { DP->SetPropertyValue(Addr, NumVal); ++Applied; }
                else if (FIntProperty* IP = CastField<FIntProperty>(Prop)) { IP->SetPropertyValue(Addr, (int32)NumVal); ++Applied; }
                else { ValStr = FString::Printf(TEXT("%f"), NumVal); Prop->ImportText_Direct(*ValStr, Addr, Expr, PPF_None); ++Applied; }
            }
            else if (Pair.Value->Type == EJson::Boolean)
            {
                if (FBoolProperty* BP = CastField<FBoolProperty>(Prop)) { BP->SetPropertyValue(Addr, Pair.Value->AsBool()); ++Applied; }
            }
            else if (Pair.Value->Type == EJson::Object)
            {
                // Object value — convert to UE property text format, e.g. {"R":1,"G":0,"B":0} → "(R=1.0,G=0.0,B=0.0)"
                const TSharedPtr<FJsonObject>& Obj = Pair.Value->AsObject();
                FString PropText = TEXT("(");
                bool bFirst = true;
                for (auto& Field : Obj->Values)
                {
                    if (!bFirst) PropText += TEXT(",");
                    double FieldNum = 0.0;
                    FString FieldStr;
                    if (Field.Value->TryGetNumber(FieldNum))
                        PropText += FString::Printf(TEXT("%s=%f"), *Field.Key, FieldNum);
                    else if (Field.Value->TryGetString(FieldStr))
                        PropText += FString::Printf(TEXT("%s=%s"), *Field.Key, *FieldStr);
                    bFirst = false;
                }
                PropText += TEXT(")");
                Prop->ImportText_Direct(*PropText, Addr, Expr, PPF_None);
                ++Applied;
            }
        }

        ForceRefreshMaterialEditor(Mat);
        Result.bSuccess = true;
        Result.Message  = FString::Printf(TEXT("Applied %d properties to '%s'"), Applied, *NodeHandle);
        return Result;
    }

    // ---- material_set_hlsl_node_io ------------------------------------------
    if (Action == TEXT("material_set_hlsl_node_io"))
    {
        FString NodeHandle;
        if (!Params->TryGetStringField(TEXT("node_id"), NodeHandle) || NodeHandle.IsEmpty())
            Params->TryGetStringField(TEXT("handle"), NodeHandle);
        if (NodeHandle.IsEmpty())
        { Result.Message = TEXT("material_set_hlsl_node_io: 'node_id' (or 'handle') required"); Result.ErrorCode = 1000; return Result; }

        UMaterialExpression* Expr = FindExpressionByHandle(Mat, NodeHandle);
        UMaterialExpressionCustom* CustomExpr = Cast<UMaterialExpressionCustom>(Expr);
        if (!CustomExpr) { Result.Message = FString::Printf(TEXT("material_set_hlsl_node_io: node '%s' is not a Custom HLSL expression"), *NodeHandle); Result.ErrorCode = Expr ? 2001 : 2000; return Result; }

        FString HlslCode;
        if (Params->TryGetStringField(TEXT("code"), HlslCode))
            CustomExpr->Code = HlslCode;

        const TArray<TSharedPtr<FJsonValue>>* InputsPtr;
        if (Params->TryGetArrayField(TEXT("inputs"), InputsPtr))
        {
            CustomExpr->Inputs.Reset();
            for (const auto& IV : *InputsPtr)
            {
                FCustomInput CI;
                CI.InputName = FName(*IV->AsString());
                CustomExpr->Inputs.Add(CI);
            }
        }

        ForceRefreshMaterialEditor(Mat);
        Result.bSuccess = true;
        Result.Message  = FString::Printf(TEXT("Updated HLSL node '%s'"), *NodeHandle);
        return Result;
    }

    // ---- material_compile_asset ---------------------------------------------
    if (Action == TEXT("material_compile_asset"))
    {
        ForceRefreshMaterialEditor(Mat);
        TArray<UPackage*> Packages = { Mat->GetOutermost() };
        bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(Packages, false);
        Result.bSuccess = bSaved;
        Result.AffectedPath = Mat->GetPathName();
        Result.Message = bSaved ? FString::Printf(TEXT("Compiled and saved: %s"), *Mat->GetName())
                                : FString::Printf(TEXT("Save failed: %s"), *Mat->GetName());
        if (!bSaved) Result.ErrorCode = 3000;
        return Result;
    }

    // ---- Phase 3 expansions ---------------------------------------------------

    // ---- material_remove_node -----------------------------------------------
    if (Action == TEXT("material_remove_node"))
    {
#if WITH_EDITOR
        FString NodeHandle;
        if (!Params->TryGetStringField(TEXT("node_id"), NodeHandle) || NodeHandle.IsEmpty())
            Params->TryGetStringField(TEXT("handle"), NodeHandle);
        if (NodeHandle.IsEmpty())
            return MakeError(TEXT("material"), Action, 1000, TEXT("'node_id' (or 'handle') required"));

        UMaterialExpression* Expr = FindExpressionByHandle(Mat, NodeHandle);
        if (!Expr)
            return MakeError(TEXT("material"), Action, 2000,
                FString::Printf(TEXT("Node '%s' not found"), *NodeHandle));

        UMaterialEditingLibrary::DeleteMaterialExpression(Mat, Expr);
        ForceRefreshMaterialEditor(Mat);

        return MakeSuccess(TEXT("material"), Action,
            FString::Printf(TEXT("Removed node '%s'"), *NodeHandle));
#else
        return MakeError(TEXT("material"), Action, 3003, TEXT("Editor-only action"));
#endif
    }

    // ---- material_disconnect_pins -------------------------------------------
    if (Action == TEXT("material_disconnect_pins"))
    {
#if WITH_EDITOR
        FString TargetHandle, InputName;
        if (!Params->TryGetStringField(TEXT("target_node"), TargetHandle) || TargetHandle.IsEmpty())
            return MakeError(TEXT("material"), Action, 1000, TEXT("'target_node' is required"));
        if (!Params->TryGetStringField(TEXT("target_input"), InputName) || InputName.IsEmpty())
            return MakeError(TEXT("material"), Action, 1000, TEXT("'target_input' is required"));

        UMaterialExpression* TargetExpr = FindExpressionByHandle(Mat, TargetHandle);
        if (!TargetExpr)
            return MakeError(TEXT("material"), Action, 2000,
                FString::Printf(TEXT("Node '%s' not found"), *TargetHandle));

        // Find the input by name via reflection
        bool bDisconnected = false;
        for (TFieldIterator<FProperty> PropIt(TargetExpr->GetClass()); PropIt; ++PropIt)
        {
            FStructProperty* SP = CastField<FStructProperty>(*PropIt);
            if (!SP || !SP->Struct) continue;
            FString SName = SP->Struct->GetName();
            if (!SName.Contains(TEXT("ExpressionInput")) && !SName.Contains(TEXT("MaterialInput"))) continue;

            if (SP->GetName().Equals(InputName, ESearchCase::IgnoreCase))
            {
                FExpressionInput* InputPtr = SP->ContainerPtrToValuePtr<FExpressionInput>(TargetExpr);
                if (InputPtr)
                {
                    InputPtr->Expression = nullptr;
                    InputPtr->OutputIndex = 0;
                    bDisconnected = true;
                }
                break;
            }
        }

        // Also check UMaterialExpressionCustom named inputs
        if (!bDisconnected)
        {
            if (UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(TargetExpr))
            {
                for (int32 i = 0; i < Custom->Inputs.Num(); ++i)
                {
                    if (Custom->Inputs[i].InputName.ToString().Equals(InputName, ESearchCase::IgnoreCase))
                    {
                        Custom->Inputs[i].Input.Expression = nullptr;
                        Custom->Inputs[i].Input.OutputIndex = 0;
                        bDisconnected = true;
                        break;
                    }
                }
            }
        }

        if (!bDisconnected)
            return MakeError(TEXT("material"), Action, 2000,
                FString::Printf(TEXT("Input '%s' not found on node '%s'"), *InputName, *TargetHandle),
                TEXT("Use material_get_pins to check available inputs"));

        ForceRefreshMaterialEditor(Mat);
        return MakeSuccess(TEXT("material"), Action,
            FString::Printf(TEXT("Disconnected input '%s' on node '%s'"), *InputName, *TargetHandle));
#else
        return MakeError(TEXT("material"), Action, 3003, TEXT("Editor-only action"));
#endif
    }

    // ---- material_set_blend_mode --------------------------------------------
    if (Action == TEXT("material_set_blend_mode"))
    {
#if WITH_EDITOR
        FString ModeStr;
        if (!Params->TryGetStringField(TEXT("blend_mode"), ModeStr) || ModeStr.IsEmpty())
            return MakeError(TEXT("material"), Action, 1000, TEXT("'blend_mode' is required (Opaque, Masked, Translucent, Additive, Modulate)"));

        EBlendMode Mode;
        if      (ModeStr.Equals(TEXT("Opaque"),      ESearchCase::IgnoreCase)) Mode = BLEND_Opaque;
        else if (ModeStr.Equals(TEXT("Masked"),       ESearchCase::IgnoreCase)) Mode = BLEND_Masked;
        else if (ModeStr.Equals(TEXT("Translucent"),  ESearchCase::IgnoreCase)) Mode = BLEND_Translucent;
        else if (ModeStr.Equals(TEXT("Additive"),     ESearchCase::IgnoreCase)) Mode = BLEND_Additive;
        else if (ModeStr.Equals(TEXT("Modulate"),     ESearchCase::IgnoreCase)) Mode = BLEND_Modulate;
        else
            return MakeError(TEXT("material"), Action, 1000,
                FString::Printf(TEXT("Unknown blend_mode '%s'"), *ModeStr),
                TEXT("Valid: Opaque, Masked, Translucent, Additive, Modulate"));

        Mat->BlendMode = Mode;
        Mat->MarkPackageDirty();
        ForceRefreshMaterialEditor(Mat);

        return MakeSuccess(TEXT("material"), Action,
            FString::Printf(TEXT("BlendMode set to %s"), *ModeStr));
#else
        return MakeError(TEXT("material"), Action, 3003, TEXT("Editor-only action"));
#endif
    }

    // ---- material_set_shading_model -----------------------------------------
    if (Action == TEXT("material_set_shading_model"))
    {
#if WITH_EDITOR
        FString ModelStr;
        if (!Params->TryGetStringField(TEXT("shading_model"), ModelStr) || ModelStr.IsEmpty())
            return MakeError(TEXT("material"), Action, 1000, TEXT("'shading_model' is required"));

        EMaterialShadingModel Model;
        if      (ModelStr.Equals(TEXT("Unlit"),           ESearchCase::IgnoreCase)) Model = MSM_Unlit;
        else if (ModelStr.Equals(TEXT("DefaultLit"),      ESearchCase::IgnoreCase)) Model = MSM_DefaultLit;
        else if (ModelStr.Equals(TEXT("Subsurface"),      ESearchCase::IgnoreCase)) Model = MSM_Subsurface;
        else if (ModelStr.Equals(TEXT("PreintegratedSkin"), ESearchCase::IgnoreCase)) Model = MSM_PreintegratedSkin;
        else if (ModelStr.Equals(TEXT("ClearCoat"),       ESearchCase::IgnoreCase)) Model = MSM_ClearCoat;
        else if (ModelStr.Equals(TEXT("SubsurfaceProfile"), ESearchCase::IgnoreCase)) Model = MSM_SubsurfaceProfile;
        else if (ModelStr.Equals(TEXT("TwoSidedFoliage"), ESearchCase::IgnoreCase)) Model = MSM_TwoSidedFoliage;
        else if (ModelStr.Equals(TEXT("Hair"),            ESearchCase::IgnoreCase)) Model = MSM_Hair;
        else if (ModelStr.Equals(TEXT("Cloth"),           ESearchCase::IgnoreCase)) Model = MSM_Cloth;
        else if (ModelStr.Equals(TEXT("Eye"),             ESearchCase::IgnoreCase)) Model = MSM_Eye;
        else if (ModelStr.Equals(TEXT("SingleLayerWater"), ESearchCase::IgnoreCase)) Model = MSM_SingleLayerWater;
        else if (ModelStr.Equals(TEXT("ThinTranslucent"), ESearchCase::IgnoreCase)) Model = MSM_ThinTranslucent;
        else if (ModelStr.Equals(TEXT("Strata"),          ESearchCase::IgnoreCase)) Model = MSM_Strata;
        else
            return MakeError(TEXT("material"), Action, 1000,
                FString::Printf(TEXT("Unknown shading_model '%s'"), *ModelStr),
                TEXT("Valid: Unlit, DefaultLit, Subsurface, PreintegratedSkin, ClearCoat, SubsurfaceProfile, TwoSidedFoliage, Hair, Cloth, Eye, SingleLayerWater, ThinTranslucent, Strata"));

        Mat->SetShadingModel(Model);
        Mat->MarkPackageDirty();
        ForceRefreshMaterialEditor(Mat);

        return MakeSuccess(TEXT("material"), Action,
            FString::Printf(TEXT("ShadingModel set to %s"), *ModelStr));
#else
        return MakeError(TEXT("material"), Action, 3003, TEXT("Editor-only action"));
#endif
    }

    // ---- material_set_two_sided ---------------------------------------------
    if (Action == TEXT("material_set_two_sided"))
    {
#if WITH_EDITOR
        bool bTwoSided = true;
        if (Params->HasField(TEXT("two_sided")))
            bTwoSided = Params->GetBoolField(TEXT("two_sided"));

        Mat->TwoSided = bTwoSided;
        Mat->MarkPackageDirty();
        ForceRefreshMaterialEditor(Mat);

        return MakeSuccess(TEXT("material"), Action,
            FString::Printf(TEXT("TwoSided set to %s"), bTwoSided ? TEXT("true") : TEXT("false")));
#else
        return MakeError(TEXT("material"), Action, 3003, TEXT("Editor-only action"));
#endif
    }

    // ---- material_get_node_properties ---------------------------------------
    if (Action == TEXT("material_get_node_properties"))
    {
#if WITH_EDITOR
        FString NodeHandle;
        if (!Params->TryGetStringField(TEXT("node_id"), NodeHandle) || NodeHandle.IsEmpty())
            Params->TryGetStringField(TEXT("handle"), NodeHandle);
        if (NodeHandle.IsEmpty())
            return MakeError(TEXT("material"), Action, 1000, TEXT("'node_id' (or 'handle') required"));

        UMaterialExpression* Expr = FindExpressionByHandle(Mat, NodeHandle);
        if (!Expr)
            return MakeError(TEXT("material"), Action, 2000,
                FString::Printf(TEXT("Node '%s' not found"), *NodeHandle));

        TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
        int32 PropCount = 0;

        for (TFieldIterator<FProperty> PropIt(Expr->GetClass()); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (!Prop) continue;
            // Only include editable properties
            if (!(Prop->PropertyFlags & CPF_Edit)) continue;

            FString ValueStr;
            const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Expr);
            Prop->ExportTextItem_Direct(ValueStr, ValuePtr, nullptr, Expr, PPF_None);

            TSharedPtr<FJsonObject> PropInfo = MakeShared<FJsonObject>();
            PropInfo->SetStringField(TEXT("type"), Prop->GetCPPType());
            PropInfo->SetStringField(TEXT("value"), ValueStr);
            PropsObj->SetObjectField(Prop->GetName(), PropInfo);
            ++PropCount;
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetObjectField(TEXT("properties"), PropsObj);
        Data->SetStringField(TEXT("node_class"), Expr->GetClass()->GetName());
        Data->SetNumberField(TEXT("count"), (double)PropCount);

        return MakeSuccess(TEXT("material"), Action,
            FString::Printf(TEXT("Node '%s' has %d editable properties"), *NodeHandle, PropCount), Data);
#else
        return MakeError(TEXT("material"), Action, 3003, TEXT("Editor-only action"));
#endif
    }

    // ---- material_list_expressions ------------------------------------------
    if (Action == TEXT("material_list_expressions"))
    {
#if WITH_EDITOR
        FString Filter;
        Params->TryGetStringField(TEXT("filter"), Filter);

        TArray<UClass*> DerivedClasses;
        GetDerivedClasses(UMaterialExpression::StaticClass(), DerivedClasses, true);

        TArray<TSharedPtr<FJsonValue>> ExprArray;
        for (UClass* Cls : DerivedClasses)
        {
            if (!Cls || Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated)) continue;

            FString ClassName = Cls->GetName();
            // Strip "MaterialExpression" prefix for cleaner output
            FString ShortName = ClassName;
            ShortName.RemoveFromStart(TEXT("MaterialExpression"));

            if (!Filter.IsEmpty() && !ShortName.Contains(Filter) && !ClassName.Contains(Filter))
                continue;

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("short_name"), ShortName);
            Entry->SetStringField(TEXT("class_name"), ClassName);
            ExprArray.Add(MakeShared<FJsonValueObject>(Entry));
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetArrayField(TEXT("expressions"), ExprArray);
        Data->SetNumberField(TEXT("count"), (double)ExprArray.Num());

        return MakeSuccess(TEXT("material"), Action,
            FString::Printf(TEXT("Found %d material expression types%s"),
                ExprArray.Num(),
                Filter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" matching '%s'"), *Filter)),
            Data);
#else
        return MakeError(TEXT("material"), Action, 3003, TEXT("Editor-only action"));
#endif
    }

    if (Action == TEXT("read_material_capture")) return Action_ReadMaterialCapture(Params);

    Result.Message = FString::Printf(
        TEXT("Unknown material action '%s'. Supported: material_set_target, material_add_node, "
             "material_connect_pins, material_connect_nodes, material_get_graph, "
             "material_set_hlsl_node_io, material_set_node_properties, material_get_pins, "
             "material_compile_asset, material_remove_node, material_disconnect_pins, "
             "material_set_blend_mode, material_set_shading_model, material_set_two_sided, "
             "material_get_node_properties, material_list_expressions, read_material_capture, "
             "create_material_function, add_parameter, bake_textures"),
        *Action);
    Result.ErrorCode = 1001;
    return Result;
}

// ---------------------------------------------------------------------------
// read_material_capture
// ---------------------------------------------------------------------------
FBridgeResult UMaterialHandler::Action_ReadMaterialCapture(TSharedPtr<FJsonObject> Params)
{
    if (!Subsystem || !Subsystem->MaterialCapture)
        return MakeError(TEXT("material"), TEXT("read_material_capture"),
            2000, TEXT("MaterialCapture subsystem unavailable"), TEXT("Ensure the plugin is fully initialized"));

    FString AssetPath, Prefix;
    const bool bHasAsset  = Params->TryGetStringField(TEXT("asset_path"), AssetPath)  && !AssetPath.IsEmpty();
    const bool bHasPrefix = Params->TryGetStringField(TEXT("prefix"),     Prefix)     && !Prefix.IsEmpty();

    if (bHasAsset)
    {
        Subsystem->MaterialCapture->ExportMaterial(AssetPath);

        FString Segment = AssetPath;
        int32 SlashIdx;
        if (Segment.FindLastChar(TEXT('/'), SlashIdx))
            Segment = Segment.RightChop(SlashIdx + 1);

        FString FilePath = FPaths::Combine(Subsystem->OutputDirectory, TEXT("materials"), Segment + TEXT(".json"));
        FString FileContent;
        FBridgeResult Res = MakeSuccess(TEXT("material"), TEXT("read_material_capture"),
            TEXT("Capture complete: ") + FilePath);
        if (FFileHelper::LoadFileToString(FileContent, *FilePath))
        {
            TSharedPtr<FJsonObject> JsonObj;
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(FileContent);
            if (FJsonSerializer::Deserialize(Reader, JsonObj) && JsonObj.IsValid())
                Res.Data = JsonObj;
        }
        return Res;
    }
    else if (bHasPrefix)
    {
        int32 Count = Subsystem->MaterialCapture->ExportMaterialsByPrefix(Prefix);
        return MakeSuccess(TEXT("material"), TEXT("read_material_capture"),
            FString::Printf(TEXT("Exported %d materials with prefix '%s'"), Count, *Prefix));
    }
    else
    {
        int32 Count = Subsystem->MaterialCapture->ExportAllMaterials();
        return MakeSuccess(TEXT("material"), TEXT("read_material_capture"),
            FString::Printf(TEXT("Exported all %d materials"), Count));
    }
}

// ---------------------------------------------------------------------------
// create_material_function
// ---------------------------------------------------------------------------
FBridgeResult UMaterialHandler::Action_CreateMaterialFunction(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
    if (!Params.IsValid())
        return MakeError(TEXT("material"), TEXT("create_material_function"), 1000, TEXT("Params object is null"));

    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(TEXT("material"), TEXT("create_material_function"), 1000,
            TEXT("'asset_path' is required (e.g. /Game/Materials/MF_MyFunc)"));

    FString Description;
    Params->TryGetStringField(TEXT("description"), Description);

    if (!FPackageName::IsValidObjectPath(AssetPath) && !AssetPath.StartsWith(TEXT("/Game/")))
        return MakeError(TEXT("material"), TEXT("create_material_function"), 1000,
            FString::Printf(TEXT("Invalid asset_path '%s'"), *AssetPath),
            TEXT("Use a /Game/-prefixed content path"));

    const FString PackagePath = FPackageName::GetLongPackagePath(AssetPath);
    const FString AssetName   = FPackageName::GetShortName(AssetPath);

    // Resolve MaterialFunctionFactoryNew at runtime (UnrealEd module)
    // UE 5.7: FindObject(ANY_PACKAGE, ...) was deprecated. For a fully-qualified
    // /Script/Module.Class path, fall straight through to LoadObject which handles
    // both the find-or-load case for native classes.
    UClass* FactClass = LoadObject<UClass>(nullptr, TEXT("/Script/UnrealEd.MaterialFunctionFactoryNew"));
    if (!FactClass)
        return MakeError(TEXT("material"), TEXT("create_material_function"), 3003,
            TEXT("MaterialFunctionFactoryNew class not found"),
            TEXT("Ensure the UnrealEd module is available (editor-only build)"));

    UFactory* Fact = NewObject<UFactory>(GetTransientPackage(), FactClass);
    if (!Fact)
        return MakeError(TEXT("material"), TEXT("create_material_function"), 3000,
            TEXT("Failed to instantiate MaterialFunctionFactoryNew"));

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    IAssetTools& AT = AssetToolsModule.Get();

    UObject* NewAsset = AT.CreateAsset(AssetName, PackagePath, UMaterialFunction::StaticClass(), Fact);
    if (!NewAsset)
        return MakeError(TEXT("material"), TEXT("create_material_function"), 3000,
            FString::Printf(TEXT("CreateAsset failed for '%s'"), *AssetPath),
            TEXT("Check that the path does not already exist and the destination folder is writable"));

    UMaterialFunction* NewMF = Cast<UMaterialFunction>(NewAsset);
    if (NewMF && !Description.IsEmpty())
    {
        NewMF->Description = Description;
    }

    if (NewAsset)
    {
        NewAsset->MarkPackageDirty();
        FAssetRegistryModule::AssetCreated(NewAsset);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("class"), TEXT("MaterialFunction"));

    FBridgeResult Res = MakeSuccess(TEXT("material"), TEXT("create_material_function"),
        FString::Printf(TEXT("Created material function: %s"), *AssetPath), Data);
    Res.AffectedPath = AssetPath;
    return Res;
#else
    return MakeError(TEXT("material"), TEXT("create_material_function"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// add_parameter
// ---------------------------------------------------------------------------
FBridgeResult UMaterialHandler::Action_AddParameter(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
    if (!Params.IsValid())
        return MakeError(TEXT("material"), TEXT("add_parameter"), 1000, TEXT("Params object is null"));

    FString MatError;
    UMaterial* Mat = GetTargetMaterial(Subsystem, Params, MatError);
    if (!Mat)
        return MakeError(TEXT("material"), TEXT("add_parameter"), 2000, MatError);

    FString ParamName;
    if (!Params->TryGetStringField(TEXT("parameter_name"), ParamName) || ParamName.IsEmpty())
        return MakeError(TEXT("material"), TEXT("add_parameter"), 1000,
            TEXT("'parameter_name' is required"));

    FString ParamType = TEXT("scalar");
    Params->TryGetStringField(TEXT("parameter_type"), ParamType);

    double X = 0.0, Y = 0.0;
    if (Params->HasField(TEXT("x"))) X = Params->GetNumberField(TEXT("x"));
    if (Params->HasField(TEXT("y"))) Y = Params->GetNumberField(TEXT("y"));

    bool bAutoCompile = false;
    if (Params->HasField(TEXT("auto_compile")))
        bAutoCompile = Params->GetBoolField(TEXT("auto_compile"));

    // Map parameter_type string to expression class
    UClass* NodeClass = nullptr;
    if (ParamType.Equals(TEXT("scalar"), ESearchCase::IgnoreCase))
        NodeClass = UMaterialExpressionScalarParameter::StaticClass();
    else if (ParamType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
        NodeClass = UMaterialExpressionVectorParameter::StaticClass();
    else if (ParamType.Equals(TEXT("texture"), ESearchCase::IgnoreCase))
        NodeClass = UMaterialExpressionTextureSampleParameter2D::StaticClass();
    else if (ParamType.Equals(TEXT("static_bool"), ESearchCase::IgnoreCase))
        NodeClass = UMaterialExpressionStaticBoolParameter::StaticClass();
    else
        return MakeError(TEXT("material"), TEXT("add_parameter"), 1000,
            FString::Printf(TEXT("Unknown parameter_type '%s'"), *ParamType),
            TEXT("Valid: scalar, vector, texture, static_bool"));

    UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(Mat, NodeClass, (int32)X, (int32)Y);
    if (!Expr)
        return MakeError(TEXT("material"), TEXT("add_parameter"), 3000,
            TEXT("CreateMaterialExpression failed"));

    // Set ParameterName — texture samplers derive from UMaterialExpressionParameter too
    if (UMaterialExpressionParameter* ParamExpr = Cast<UMaterialExpressionParameter>(Expr))
    {
        ParamExpr->ParameterName = FName(*ParamName);
    }
    else if (UMaterialExpressionTextureSampleParameter2D* TexParam = Cast<UMaterialExpressionTextureSampleParameter2D>(Expr))
    {
        // TextureSampleParameter2D inherits from UMaterialExpressionTextureSampleParameter -> UMaterialExpressionTextureBase
        // ParameterName lives on UMaterialExpressionTextureSampleParameter
        TexParam->ParameterName = FName(*ParamName);
    }
    else
    {
        // Fallback via reflection
        if (FProperty* NameProp = Expr->GetClass()->FindPropertyByName(TEXT("ParameterName")))
        {
            if (FNameProperty* NP = CastField<FNameProperty>(NameProp))
            {
                NP->SetPropertyValue(NP->ContainerPtrToValuePtr<void>(Expr), FName(*ParamName));
            }
        }
    }

    Mat->MarkPackageDirty();
    ForceRefreshMaterialEditor(Mat);

    if (bAutoCompile)
    {
        UMaterialEditingLibrary::RecompileMaterial(Mat);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("handle"), Expr->GetName());
    Data->SetStringField(TEXT("parameter_name"), ParamName);
    Data->SetStringField(TEXT("parameter_type"), ParamType);
    Data->SetStringField(TEXT("class"), Expr->GetClass()->GetName());

    FBridgeResult Res = MakeSuccess(TEXT("material"), TEXT("add_parameter"),
        FString::Printf(TEXT("Added %s parameter '%s' (handle: %s)"), *ParamType, *ParamName, *Expr->GetName()),
        Data);
    Res.AffectedPath = Expr->GetName();
    return Res;
#else
    return MakeError(TEXT("material"), TEXT("add_parameter"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// bake_textures
// ---------------------------------------------------------------------------
FBridgeResult UMaterialHandler::Action_BakeTextures(TSharedPtr<FJsonObject> Params)
{
#if WITH_EDITOR
    if (!Params.IsValid())
        return MakeError(TEXT("material"), TEXT("bake_textures"), 1000, TEXT("Params object is null"));

    FString OutputPath;
    if (!Params->TryGetStringField(TEXT("output_path"), OutputPath) || OutputPath.IsEmpty())
        return MakeError(TEXT("material"), TEXT("bake_textures"), 1000,
            TEXT("'output_path' is required (folder to write baked textures)"));

    int32 Size = 512;
    if (Params->HasField(TEXT("size")))
        Size = (int32)Params->GetNumberField(TEXT("size"));

    bool bIncludeDiffuse = true;
    if (Params->HasField(TEXT("include_diffuse")))
        bIncludeDiffuse = Params->GetBoolField(TEXT("include_diffuse"));

    bool bIncludeNormal = false;
    if (Params->HasField(TEXT("include_normal")))
        bIncludeNormal = Params->GetBoolField(TEXT("include_normal"));

    // Resolve material target (optional — we just surface it in the response)
    FString MatError;
    UMaterial* Mat = GetTargetMaterial(Subsystem, Params, MatError);
    FString MatPath = Mat ? Mat->GetPathName() : FString();

    // Check if MaterialBaking module is available
    if (!FModuleManager::Get().ModuleExists(TEXT("MaterialBaking")))
    {
        return MakeError(TEXT("material"), TEXT("bake_textures"), 3003,
            TEXT("bake_textures requires MaterialBaking plugin/module to be enabled"),
            TEXT("Enable the MaterialBaking module in the project's .uproject or build configuration"));
    }

    // Ensure it is loaded (stateless probe — we don't dispatch the bake yet)
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("MaterialBaking")))
    {
        if (!FModuleManager::Get().LoadModule(TEXT("MaterialBaking")))
        {
            return MakeError(TEXT("material"), TEXT("bake_textures"), 3003,
                TEXT("bake_textures requires MaterialBaking plugin/module to be enabled"),
                TEXT("Failed to load MaterialBaking module at runtime"));
        }
    }

    // Register async job in session store (client polls via project/get_job_status)
    const FString JobId = FBridgeSessionStore::Get().CreateJob(TEXT("material/bake_textures"));
    FBridgeSessionStore::Get().UpdateJob(JobId, EBridgeJobStatus::Running, 0, TEXT("Bake queued"));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("job_id"), JobId);
    Data->SetStringField(TEXT("status"), TEXT("queued"));
    Data->SetStringField(TEXT("note"), TEXT("Full bake dispatch pending MaterialBaking worker wiring. Poll project/get_job_status for progress."));
    Data->SetStringField(TEXT("output_path"), OutputPath);
    Data->SetNumberField(TEXT("size"), (double)Size);
    Data->SetBoolField(TEXT("include_diffuse"), bIncludeDiffuse);
    Data->SetBoolField(TEXT("include_normal"), bIncludeNormal);
    if (!MatPath.IsEmpty())
        Data->SetStringField(TEXT("asset_path"), MatPath);

    return MakeSuccess(TEXT("material"), TEXT("bake_textures"),
        FString::Printf(TEXT("Bake queued: job_id=%s (size=%d, diffuse=%s, normal=%s)"),
            *JobId, Size,
            bIncludeDiffuse ? TEXT("true") : TEXT("false"),
            bIncludeNormal  ? TEXT("true") : TEXT("false")),
        Data);
#else
    return MakeError(TEXT("material"), TEXT("bake_textures"), 3003, TEXT("Editor-only action"));
#endif
}

// ---------------------------------------------------------------------------
// GetActionSchemas
// ---------------------------------------------------------------------------
TSharedPtr<FJsonObject> UMaterialHandler::GetActionSchemas() const
{
    auto P = [](const FString& Type, bool bRequired, const FString& Desc) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("type"), Type);
        O->SetBoolField(TEXT("required"), bRequired);
        O->SetStringField(TEXT("desc"), Desc);
        return O;
    };

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

    // material_set_target
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Set the active material target for subsequent operations. Creates the material if it does not exist."));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path of the material asset")));
        Params->SetObjectField(TEXT("create_if_missing"), P(TEXT("bool"), false, TEXT("Create the material if not found (default true)")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_set_target"), A);
    }

    // material_add_node
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Add a material expression node to the graph"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path (or uses AttentionManager target)")));
        Params->SetObjectField(TEXT("node_type"), P(TEXT("string"), true, TEXT("Expression class name (e.g. Add, Multiply, TextureSample, Custom)")));
        Params->SetObjectField(TEXT("node_name"), P(TEXT("string"), false, TEXT("Description/label for the node")));
        Params->SetObjectField(TEXT("x"), P(TEXT("int"), false, TEXT("Editor X position (default -400)")));
        Params->SetObjectField(TEXT("y"), P(TEXT("int"), false, TEXT("Editor Y position (auto-staggered if omitted)")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_add_node"), A);
    }

    // material_connect_pins
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Connect an output pin of one node to an input pin of another (or to the root material node)"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path (or uses AttentionManager target)")));
        Params->SetObjectField(TEXT("from_node"), P(TEXT("string"), true, TEXT("Source node handle/name")));
        Params->SetObjectField(TEXT("from_pin"), P(TEXT("string"), false, TEXT("Source output pin name (first output if omitted)")));
        Params->SetObjectField(TEXT("to_node"), P(TEXT("string"), true, TEXT("Target node handle/name (use Root/Output/Material for material root)")));
        Params->SetObjectField(TEXT("to_pin"), P(TEXT("string"), false, TEXT("Target input pin name (auto-resolved if omitted)")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_connect_pins"), A);
    }

    // material_connect_nodes
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Simplified node connection — connects first output to first input, no pin names needed"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        Params->SetObjectField(TEXT("from_node"), P(TEXT("string"), true, TEXT("Source node handle/name")));
        Params->SetObjectField(TEXT("to_node"), P(TEXT("string"), true, TEXT("Target node handle/name")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_connect_nodes"), A);
    }

    // material_disconnect_pins
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Disconnect an input pin on a target node"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        Params->SetObjectField(TEXT("target_node"), P(TEXT("string"), true, TEXT("Node handle whose input to disconnect")));
        Params->SetObjectField(TEXT("target_input"), P(TEXT("string"), true, TEXT("Input pin name to disconnect")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_disconnect_pins"), A);
    }

    // material_get_pins
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("List all input and output pins on a material expression node"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        Params->SetObjectField(TEXT("node_id"), P(TEXT("string"), true, TEXT("Node handle/name (alias: handle)")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_get_pins"), A);
    }

    // material_set_node_properties
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Set reflected properties on a material expression node"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        Params->SetObjectField(TEXT("node_id"), P(TEXT("string"), true, TEXT("Node handle/name (alias: handle)")));
        Params->SetObjectField(TEXT("properties"), P(TEXT("object"), true, TEXT("Key-value pairs of property names to values")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_set_node_properties"), A);
    }

    // material_get_node_properties
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Get all editable properties and their values from a material expression node"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        Params->SetObjectField(TEXT("node_id"), P(TEXT("string"), true, TEXT("Node handle/name (alias: handle)")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_get_node_properties"), A);
    }

    // material_get_graph
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Return a JSON list of all nodes in the material graph with handles, classes, and positions"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_get_graph"), A);
    }

    // material_compile_asset
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Compile and save the material asset"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_compile_asset"), A);
    }

    // material_remove_node
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Remove a material expression node from the graph"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        Params->SetObjectField(TEXT("node_id"), P(TEXT("string"), true, TEXT("Node handle/name to remove (alias: handle)")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_remove_node"), A);
    }

    // material_set_blend_mode
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Set the material blend mode"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        Params->SetObjectField(TEXT("blend_mode"), P(TEXT("string"), true, TEXT("Opaque, Masked, Translucent, Additive, or Modulate")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_set_blend_mode"), A);
    }

    // material_set_shading_model
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Set the material shading model"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        Params->SetObjectField(TEXT("shading_model"), P(TEXT("string"), true, TEXT("Unlit, DefaultLit, Subsurface, ClearCoat, Hair, Cloth, Eye, Strata, etc.")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_set_shading_model"), A);
    }

    // material_set_two_sided
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Set whether the material is two-sided"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        Params->SetObjectField(TEXT("two_sided"), P(TEXT("bool"), false, TEXT("Enable two-sided rendering (default true)")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_set_two_sided"), A);
    }

    // material_list_expressions
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("List all available MaterialExpression classes for use with material_add_node"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        Params->SetObjectField(TEXT("filter"), P(TEXT("string"), false, TEXT("Filter expression names by substring")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_list_expressions"), A);
    }

    // material_set_hlsl_node_io
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Configure inputs and HLSL code on a Custom expression node"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path")));
        Params->SetObjectField(TEXT("node_id"), P(TEXT("string"), true, TEXT("Custom HLSL node handle/name (alias: handle)")));
        Params->SetObjectField(TEXT("inputs"), P(TEXT("array<string>"), false, TEXT("Array of input pin names")));
        Params->SetObjectField(TEXT("code"), P(TEXT("string"), false, TEXT("HLSL code to set on the Custom node")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("material_set_hlsl_node_io"), A);
    }

    // read_material_capture
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Trigger material capture export and return the JSON file contents. Without params, exports all materials and returns a count."));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Single material asset path to export (e.g. /Game/Materials/M_Rock)")));
        Params->SetObjectField(TEXT("prefix"), P(TEXT("string"), false, TEXT("Export all materials matching this prefix; returns count")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("read_material_capture"), A);
    }

    // create_material_function
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Create a new UMaterialFunction asset at the given content path"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), true, TEXT("Content path for the new material function (e.g. /Game/Materials/MF_MyFunc)")));
        Params->SetObjectField(TEXT("description"), P(TEXT("string"), false, TEXT("Optional description text for the material function")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("create_material_function"), A);
    }

    // add_parameter
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Add a material parameter expression node (scalar, vector, texture, or static bool) to the target material"));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path (or uses AttentionManager target)")));
        Params->SetObjectField(TEXT("parameter_name"), P(TEXT("string"), true, TEXT("Name of the parameter (used as the ParameterName)")));
        Params->SetObjectField(TEXT("parameter_type"), P(TEXT("string"), false, TEXT("scalar | vector | texture | static_bool (default: scalar)")));
        Params->SetObjectField(TEXT("x"), P(TEXT("number"), false, TEXT("Editor X position (default 0)")));
        Params->SetObjectField(TEXT("y"), P(TEXT("number"), false, TEXT("Editor Y position (default 0)")));
        Params->SetObjectField(TEXT("auto_compile"), P(TEXT("bool"), false, TEXT("Call RecompileMaterial after adding (default false)")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("add_parameter"), A);
    }

    // bake_textures
    {
        TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
        A->SetStringField(TEXT("desc"), TEXT("Queue an async texture bake for the target material. Returns a job_id. Requires the MaterialBaking module. Full dispatch pending BridgeSessionStore."));
        TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
        Params->SetObjectField(TEXT("asset_path"), P(TEXT("string"), false, TEXT("Material asset path (or uses AttentionManager target)")));
        Params->SetObjectField(TEXT("output_path"), P(TEXT("string"), true, TEXT("Folder to write baked textures into")));
        Params->SetObjectField(TEXT("size"), P(TEXT("int"), false, TEXT("Output texture size in pixels (default 512)")));
        Params->SetObjectField(TEXT("include_diffuse"), P(TEXT("bool"), false, TEXT("Bake the diffuse/base color channel (default true)")));
        Params->SetObjectField(TEXT("include_normal"), P(TEXT("bool"), false, TEXT("Bake the normal channel (default false)")));
        A->SetObjectField(TEXT("params"), Params);
        Root->SetObjectField(TEXT("bake_textures"), A);
    }

    return Root;
}
