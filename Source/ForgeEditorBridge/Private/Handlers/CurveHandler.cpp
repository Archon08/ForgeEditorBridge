#include "Handlers/CurveHandler.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Curves/CurveLinearColor.h"
#include "Engine/CurveTable.h"
#include "Curves/RichCurve.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Editor.h"

#ifdef DOMAIN
#undef DOMAIN
#endif
static const FString DOMAIN = TEXT("curve");

namespace
{
    UObject* LoadCurveAsset(const FString& AssetPath)
    {
        if (UObject* Obj = LoadObject<UObject>(nullptr, *AssetPath)) return Obj;
        const FString Suffix = AssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(AssetPath);
        return LoadObject<UObject>(nullptr, *Suffix);
    }

    int32 ChannelToIndex(const FString& Channel, int32 Max)
    {
        if (Channel.IsEmpty()) return 0;
        const FString C = Channel.ToLower();
        if (Max <= 4 && (C == TEXT("r") || C == TEXT("x"))) return 0;
        if (Max <= 4 && (C == TEXT("g") || C == TEXT("y"))) return 1;
        if (Max <= 4 && (C == TEXT("b") || C == TEXT("z"))) return 2;
        if (Max == 4 && (C == TEXT("a") || C == TEXT("w"))) return 3;
        return -1;
    }

    FRichCurve* GetTargetRichCurve(UObject* Obj, const FString& Channel, FString& OutErr)
    {
        if (UCurveFloat* CF = Cast<UCurveFloat>(Obj))
        {
            return &CF->FloatCurve;
        }
        if (UCurveVector* CV = Cast<UCurveVector>(Obj))
        {
            const int32 Idx = ChannelToIndex(Channel, 3);
            if (Idx < 0) { OutErr = TEXT("CurveVector requires channel x|y|z"); return nullptr; }
            return &CV->FloatCurves[Idx];
        }
        if (UCurveLinearColor* CC = Cast<UCurveLinearColor>(Obj))
        {
            const int32 Idx = ChannelToIndex(Channel, 4);
            if (Idx < 0) { OutErr = TEXT("CurveLinearColor requires channel r|g|b|a"); return nullptr; }
            return &CC->FloatCurves[Idx];
        }
        OutErr = TEXT("Asset is not a UCurveFloat/UCurveVector/UCurveLinearColor");
        return nullptr;
    }

    ERichCurveInterpMode ParseInterp(const FString& Mode)
    {
        const FString M = Mode.ToLower();
        if (M == TEXT("constant")) return RCIM_Constant;
        if (M == TEXT("linear"))   return RCIM_Linear;
        if (M == TEXT("cubic"))    return RCIM_Cubic;
        if (M == TEXT("none"))     return RCIM_None;
        return RCIM_Cubic;
    }

    ERichCurveTangentMode ParseTangentMode(const FString& Mode)
    {
        const FString M = Mode.ToLower();
        if (M == TEXT("user"))  return RCTM_User;
        if (M == TEXT("break")) return RCTM_Break;
        return RCTM_Auto;
    }

    ERichCurveExtrapolation ParseExtrap(const FString& Mode)
    {
        const FString M = Mode.ToLower();
        if (M == TEXT("cycle"))             return RCCE_Cycle;
        if (M == TEXT("cycle_with_offset")) return RCCE_CycleWithOffset;
        if (M == TEXT("oscillate"))         return RCCE_Oscillate;
        if (M == TEXT("linear"))            return RCCE_Linear;
        if (M == TEXT("constant"))          return RCCE_Constant;
        return RCCE_None;
    }

    FString InterpToString(ERichCurveInterpMode M)
    {
        switch (M) { case RCIM_Constant: return TEXT("constant");
                     case RCIM_Linear:   return TEXT("linear");
                     case RCIM_Cubic:    return TEXT("cubic");
                     default:            return TEXT("none"); }
    }

    UObject* CreateCurveAsset(UClass* Class, const FString& AssetPath, FString& OutErr)
    {
        const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
        const FString AssetName   = FPackageName::GetLongPackageAssetName(PackageName);
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package) { OutErr = FString::Printf(TEXT("CreatePackage failed: %s"), *PackageName); return nullptr; }
        Package->FullyLoad();

        UObject* NewObj = NewObject<UObject>(Package, Class, FName(*AssetName),
            RF_Public | RF_Standalone | RF_Transactional);
        if (!NewObj) { OutErr = TEXT("NewObject failed"); return nullptr; }

        FAssetRegistryModule::AssetCreated(NewObj);
        Package->MarkPackageDirty();
        return NewObj;
    }
}

FBridgeResult UCurveHandler::HandleCommand(const FString& Action, TSharedPtr<FJsonObject> Params)
{
    if (!Params.IsValid())
    {
        return MakeError(DOMAIN, Action, 1000, TEXT("Params object is null"));
    }

    if (Action == TEXT("create_curve_float"))   return Action_CreateCurveFloat(Params);
    if (Action == TEXT("create_curve_vector"))  return Action_CreateCurveVector(Params);
    if (Action == TEXT("create_curve_color"))   return Action_CreateCurveColor(Params);
    if (Action == TEXT("create_curve_table"))   return Action_CreateCurveTable(Params);
    if (Action == TEXT("add_key"))              return Action_AddKey(Params);
    if (Action == TEXT("remove_key"))           return Action_RemoveKey(Params);
    if (Action == TEXT("set_key_tangent"))      return Action_SetKeyTangent(Params);
    if (Action == TEXT("set_key_value"))        return Action_SetKeyValue(Params);
    if (Action == TEXT("eval"))                 return Action_Eval(Params);
    if (Action == TEXT("list_keys"))            return Action_ListKeys(Params);
    if (Action == TEXT("clear_keys"))           return Action_ClearKeys(Params);
    if (Action == TEXT("set_extrap"))           return Action_SetExtrap(Params);

    return MakeUnknownAction(DOMAIN, Action,
        TEXT("create_curve_float, create_curve_vector, create_curve_color, create_curve_table, add_key, remove_key, set_key_tangent, set_key_value, eval, list_keys, clear_keys, set_extrap"));
}

FBridgeResult UCurveHandler::Action_CreateCurveFloat(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_curve_float"), 1000, TEXT("'asset_path' is required"));
    FString Err;
    UObject* Obj = CreateCurveAsset(UCurveFloat::StaticClass(), AssetPath, Err);
    if (!Obj) return MakeError(DOMAIN, TEXT("create_curve_float"), 3000, Err);
    return MakeSuccess(DOMAIN, TEXT("create_curve_float"),
        FString::Printf(TEXT("Created UCurveFloat at '%s'"), *AssetPath));
}

FBridgeResult UCurveHandler::Action_CreateCurveVector(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_curve_vector"), 1000, TEXT("'asset_path' is required"));
    FString Err;
    UObject* Obj = CreateCurveAsset(UCurveVector::StaticClass(), AssetPath, Err);
    if (!Obj) return MakeError(DOMAIN, TEXT("create_curve_vector"), 3000, Err);
    return MakeSuccess(DOMAIN, TEXT("create_curve_vector"),
        FString::Printf(TEXT("Created UCurveVector at '%s'"), *AssetPath));
}

FBridgeResult UCurveHandler::Action_CreateCurveColor(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_curve_color"), 1000, TEXT("'asset_path' is required"));
    FString Err;
    UObject* Obj = CreateCurveAsset(UCurveLinearColor::StaticClass(), AssetPath, Err);
    if (!Obj) return MakeError(DOMAIN, TEXT("create_curve_color"), 3000, Err);
    return MakeSuccess(DOMAIN, TEXT("create_curve_color"),
        FString::Printf(TEXT("Created UCurveLinearColor at '%s'"), *AssetPath));
}

FBridgeResult UCurveHandler::Action_CreateCurveTable(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("create_curve_table"), 1000, TEXT("'asset_path' is required"));
    FString Err;
    UObject* Obj = CreateCurveAsset(UCurveTable::StaticClass(), AssetPath, Err);
    if (!Obj) return MakeError(DOMAIN, TEXT("create_curve_table"), 3000, Err);
    return MakeSuccess(DOMAIN, TEXT("create_curve_table"),
        FString::Printf(TEXT("Created UCurveTable at '%s'"), *AssetPath));
}

FBridgeResult UCurveHandler::Action_AddKey(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Curve Add Key"));
    FString AssetPath, Channel, Interp;
    double Time = 0.0, Value = 0.0;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("add_key"), 1000, TEXT("'asset_path' is required"));
    if (!Params->TryGetNumberField(TEXT("time"), Time))
        return MakeError(DOMAIN, TEXT("add_key"), 1000, TEXT("'time' is required"));
    if (!Params->TryGetNumberField(TEXT("value"), Value))
        return MakeError(DOMAIN, TEXT("add_key"), 1000, TEXT("'value' is required"));
    Params->TryGetStringField(TEXT("channel"), Channel);
    Params->TryGetStringField(TEXT("interp"), Interp);

    UObject* Obj = LoadCurveAsset(AssetPath);
    if (!Obj) return MakeError(DOMAIN, TEXT("add_key"), 2000,
        FString::Printf(TEXT("Curve not found: %s"), *AssetPath));

    FString Err;
    FRichCurve* Curve = GetTargetRichCurve(Obj, Channel, Err);
    if (!Curve) return MakeError(DOMAIN, TEXT("add_key"), 2001, Err);

    FKeyHandle Key = Curve->AddKey((float)Time, (float)Value);
    Curve->SetKeyInterpMode(Key, ParseInterp(Interp));
    Obj->MarkPackageDirty();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("time"), Time);
    Data->SetNumberField(TEXT("value"), Value);
    return MakeSuccess(DOMAIN, TEXT("add_key"),
        FString::Printf(TEXT("Added key (t=%.4f, v=%.4f)"), Time, Value), Data);
}

FBridgeResult UCurveHandler::Action_RemoveKey(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Curve Remove Key"));
    FString AssetPath, Channel;
    double Time = 0.0;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("remove_key"), 1000, TEXT("'asset_path' is required"));
    if (!Params->TryGetNumberField(TEXT("time"), Time))
        return MakeError(DOMAIN, TEXT("remove_key"), 1000, TEXT("'time' is required"));
    Params->TryGetStringField(TEXT("channel"), Channel);

    UObject* Obj = LoadCurveAsset(AssetPath);
    if (!Obj) return MakeError(DOMAIN, TEXT("remove_key"), 2000, TEXT("Curve not found"));
    FString Err;
    FRichCurve* Curve = GetTargetRichCurve(Obj, Channel, Err);
    if (!Curve) return MakeError(DOMAIN, TEXT("remove_key"), 2001, Err);

    const float Tolerance = 1e-4f;
    int32 Removed = 0;
    TArray<FKeyHandle> ToDelete;
    for (auto It = Curve->GetKeyHandleIterator(); It; ++It)
    {
        if (FMath::IsNearlyEqual(Curve->GetKeyTime(*It), (float)Time, Tolerance))
        {
            ToDelete.Add(*It);
        }
    }
    for (FKeyHandle H : ToDelete) { Curve->DeleteKey(H); ++Removed; }
    if (Removed > 0) Obj->MarkPackageDirty();

    return MakeSuccess(DOMAIN, TEXT("remove_key"),
        FString::Printf(TEXT("Removed %d key(s) at t=%.4f"), Removed, Time));
}

FBridgeResult UCurveHandler::Action_SetKeyTangent(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Curve Set Tangent"));
    FString AssetPath, Channel, TangentMode;
    double Time = 0.0, Arrive = 0.0, Leave = 0.0;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_key_tangent"), 1000, TEXT("'asset_path' is required"));
    if (!Params->TryGetNumberField(TEXT("time"), Time))
        return MakeError(DOMAIN, TEXT("set_key_tangent"), 1000, TEXT("'time' is required"));
    Params->TryGetNumberField(TEXT("arrive_tangent"), Arrive);
    Params->TryGetNumberField(TEXT("leave_tangent"),  Leave);
    Params->TryGetStringField(TEXT("tangent_mode"),   TangentMode);
    Params->TryGetStringField(TEXT("channel"), Channel);

    UObject* Obj = LoadCurveAsset(AssetPath);
    if (!Obj) return MakeError(DOMAIN, TEXT("set_key_tangent"), 2000, TEXT("Curve not found"));
    FString Err;
    FRichCurve* Curve = GetTargetRichCurve(Obj, Channel, Err);
    if (!Curve) return MakeError(DOMAIN, TEXT("set_key_tangent"), 2001, Err);

    const float Tol = 1e-4f;
    int32 Set = 0;
    for (auto It = Curve->GetKeyHandleIterator(); It; ++It)
    {
        if (FMath::IsNearlyEqual(Curve->GetKeyTime(*It), (float)Time, Tol))
        {
            FRichCurveKey& K = Curve->GetKey(*It);
            K.ArriveTangent = (float)Arrive;
            K.LeaveTangent  = (float)Leave;
            K.TangentMode   = ParseTangentMode(TangentMode);
            ++Set;
        }
    }
    if (Set == 0)
    {
        return MakeError(DOMAIN, TEXT("set_key_tangent"), 2000,
            FString::Printf(TEXT("No key at t=%.4f"), Time));
    }
    Obj->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("set_key_tangent"),
        FString::Printf(TEXT("Set tangent on %d key(s)"), Set));
}

FBridgeResult UCurveHandler::Action_SetKeyValue(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Curve Set Key Value"));
    FString AssetPath, Channel;
    double Time = 0.0, Value = 0.0;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_key_value"), 1000, TEXT("'asset_path' is required"));
    if (!Params->TryGetNumberField(TEXT("time"), Time))
        return MakeError(DOMAIN, TEXT("set_key_value"), 1000, TEXT("'time' is required"));
    if (!Params->TryGetNumberField(TEXT("value"), Value))
        return MakeError(DOMAIN, TEXT("set_key_value"), 1000, TEXT("'value' is required"));
    Params->TryGetStringField(TEXT("channel"), Channel);

    UObject* Obj = LoadCurveAsset(AssetPath);
    if (!Obj) return MakeError(DOMAIN, TEXT("set_key_value"), 2000, TEXT("Curve not found"));
    FString Err;
    FRichCurve* Curve = GetTargetRichCurve(Obj, Channel, Err);
    if (!Curve) return MakeError(DOMAIN, TEXT("set_key_value"), 2001, Err);

    const float Tol = 1e-4f;
    int32 Set = 0;
    for (auto It = Curve->GetKeyHandleIterator(); It; ++It)
    {
        if (FMath::IsNearlyEqual(Curve->GetKeyTime(*It), (float)Time, Tol))
        {
            Curve->SetKeyValue(*It, (float)Value);
            ++Set;
        }
    }
    if (Set == 0)
    {
        return MakeError(DOMAIN, TEXT("set_key_value"), 2000,
            FString::Printf(TEXT("No key at t=%.4f"), Time));
    }
    Obj->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("set_key_value"),
        FString::Printf(TEXT("Set value on %d key(s) to %.4f"), Set, Value));
}

FBridgeResult UCurveHandler::Action_Eval(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath, Channel;
    double Time = 0.0;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("eval"), 1000, TEXT("'asset_path' is required"));
    if (!Params->TryGetNumberField(TEXT("time"), Time))
        return MakeError(DOMAIN, TEXT("eval"), 1000, TEXT("'time' is required"));
    Params->TryGetStringField(TEXT("channel"), Channel);

    UObject* Obj = LoadCurveAsset(AssetPath);
    if (!Obj) return MakeError(DOMAIN, TEXT("eval"), 2000, TEXT("Curve not found"));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("time"), Time);

    if (UCurveFloat* CF = Cast<UCurveFloat>(Obj))
    {
        Data->SetNumberField(TEXT("value"), CF->GetFloatValue((float)Time));
    }
    else if (UCurveVector* CV = Cast<UCurveVector>(Obj))
    {
        const FVector V = CV->GetVectorValue((float)Time);
        Data->SetNumberField(TEXT("x"), V.X);
        Data->SetNumberField(TEXT("y"), V.Y);
        Data->SetNumberField(TEXT("z"), V.Z);
    }
    else if (UCurveLinearColor* CC = Cast<UCurveLinearColor>(Obj))
    {
        const FLinearColor C = CC->GetLinearColorValue((float)Time);
        Data->SetNumberField(TEXT("r"), C.R);
        Data->SetNumberField(TEXT("g"), C.G);
        Data->SetNumberField(TEXT("b"), C.B);
        Data->SetNumberField(TEXT("a"), C.A);
    }
    else
    {
        return MakeError(DOMAIN, TEXT("eval"), 2001, TEXT("Asset type not evaluable"));
    }
    return MakeSuccess(DOMAIN, TEXT("eval"), TEXT("Evaluated"), Data);
}

FBridgeResult UCurveHandler::Action_ListKeys(TSharedPtr<FJsonObject> Params)
{
    FString AssetPath, Channel;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("list_keys"), 1000, TEXT("'asset_path' is required"));
    Params->TryGetStringField(TEXT("channel"), Channel);

    UObject* Obj = LoadCurveAsset(AssetPath);
    if (!Obj) return MakeError(DOMAIN, TEXT("list_keys"), 2000, TEXT("Curve not found"));
    FString Err;
    FRichCurve* Curve = GetTargetRichCurve(Obj, Channel, Err);
    if (!Curve) return MakeError(DOMAIN, TEXT("list_keys"), 2001, Err);

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (auto It = Curve->GetKeyHandleIterator(); It; ++It)
    {
        const FRichCurveKey& K = Curve->GetKey(*It);
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("time"), K.Time);
        Entry->SetNumberField(TEXT("value"), K.Value);
        Entry->SetStringField(TEXT("interp"), InterpToString(K.InterpMode));
        Entry->SetNumberField(TEXT("arrive_tangent"), K.ArriveTangent);
        Entry->SetNumberField(TEXT("leave_tangent"), K.LeaveTangent);
        Arr.Add(MakeShared<FJsonValueObject>(Entry));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("keys"), Arr);
    Data->SetNumberField(TEXT("count"), Arr.Num());
    return MakeSuccess(DOMAIN, TEXT("list_keys"),
        FString::Printf(TEXT("%d key(s)"), Arr.Num()), Data);
}

FBridgeResult UCurveHandler::Action_ClearKeys(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Curve Clear Keys"));
    FString AssetPath, Channel;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("clear_keys"), 1000, TEXT("'asset_path' is required"));
    Params->TryGetStringField(TEXT("channel"), Channel);

    UObject* Obj = LoadCurveAsset(AssetPath);
    if (!Obj) return MakeError(DOMAIN, TEXT("clear_keys"), 2000, TEXT("Curve not found"));
    FString Err;
    FRichCurve* Curve = GetTargetRichCurve(Obj, Channel, Err);
    if (!Curve) return MakeError(DOMAIN, TEXT("clear_keys"), 2001, Err);

    const int32 Before = Curve->Keys.Num();
    Curve->Reset();
    Obj->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("clear_keys"),
        FString::Printf(TEXT("Cleared %d key(s)"), Before));
}

FBridgeResult UCurveHandler::Action_SetExtrap(TSharedPtr<FJsonObject> Params)
{
    auto Tx = BeginTransaction(TEXT("Curve Set Extrap"));
    FString AssetPath, Channel, Pre, Post;
    if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
        return MakeError(DOMAIN, TEXT("set_extrap"), 1000, TEXT("'asset_path' is required"));
    Params->TryGetStringField(TEXT("pre"),  Pre);
    Params->TryGetStringField(TEXT("post"), Post);
    Params->TryGetStringField(TEXT("channel"), Channel);

    UObject* Obj = LoadCurveAsset(AssetPath);
    if (!Obj) return MakeError(DOMAIN, TEXT("set_extrap"), 2000, TEXT("Curve not found"));
    FString Err;
    FRichCurve* Curve = GetTargetRichCurve(Obj, Channel, Err);
    if (!Curve) return MakeError(DOMAIN, TEXT("set_extrap"), 2001, Err);

    if (!Pre.IsEmpty())  Curve->PreInfinityExtrap  = ParseExtrap(Pre);
    if (!Post.IsEmpty()) Curve->PostInfinityExtrap = ParseExtrap(Post);
    Obj->MarkPackageDirty();
    return MakeSuccess(DOMAIN, TEXT("set_extrap"), TEXT("Extrapolation updated"));
}