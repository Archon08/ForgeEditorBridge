#include "Handlers/BlueprintGraphLayout.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"

#include "K2Node_Event.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"

#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Layout constants. Tuned to resemble the in-editor Kismet autoformatter.
// ---------------------------------------------------------------------------
namespace
{
    constexpr int32   GRID_SNAP        = 16;
    constexpr float   GAP_X            = 96.f;  // column gap
    constexpr float   GAP_Y            = 48.f;  // row gap
    constexpr float   TITLE_HEIGHT     = 28.f;
    constexpr float   PIN_HEIGHT       = 18.f;
    constexpr float   NODE_PADDING     = 16.f;
    constexpr float   MIN_WIDTH        = 180.f;
    constexpr float   CHAR_PX          = 7.f;   // rough px per char for pin label
    constexpr int32   KNOT_MIN_SPAN    = 3;     // wires spanning ≥N layers get a reroute
    constexpr float   COMMENT_PAD      = 40.f;  // comment box padding around members
    constexpr int32   COMMENT_FONT     = 18;
    constexpr float   COMMENT_MIN_ALPHA= 0.25f;

    static bool IsExecPin(const UEdGraphPin* Pin)
    {
        return Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
    }

    static bool IsEntryNode(const UEdGraphNode* N)
    {
        return N && (N->IsA<UK2Node_Event>()
                   || N->IsA<UK2Node_CustomEvent>()
                   || N->IsA<UK2Node_FunctionEntry>());
    }

    static bool NodeHasExecPin(const UEdGraphNode* N)
    {
        if (!N) return false;
        for (const UEdGraphPin* P : N->Pins) if (IsExecPin(P)) return true;
        return false;
    }

    // Estimate node size without a live Slate widget. Matches community autolayout tools.
    static FVector2f EstimateNodeSize(const UEdGraphNode* N)
    {
        int32 PinCount = 0;
        int32 LongestLabel = 0;
        if (N)
        {
            PinCount = N->Pins.Num();
            for (const UEdGraphPin* P : N->Pins)
            {
                const int32 L = P ? P->PinName.ToString().Len() : 0;
                if (L > LongestLabel) LongestLabel = L;
            }
        }
        const float W = FMath::Max(MIN_WIDTH, 32.f + (float)LongestLabel * CHAR_PX);
        const float H = TITLE_HEIGHT + (float)PinCount * PIN_HEIGHT + NODE_PADDING;
        return FVector2f(W, H);
    }

    static int32 SnapToGridInt(float V)
    {
        return FMath::RoundToInt(V / (float)GRID_SNAP) * GRID_SNAP;
    }

    // Longest-path layering over PC_Exec edges starting from entry nodes.
    static void AssignLayers(UEdGraph* Graph, TMap<UEdGraphNode*, int32>& OutLayer)
    {
        OutLayer.Reset();
        TArray<UEdGraphNode*> Frontier;

        for (UEdGraphNode* N : Graph->Nodes)
        {
            if (IsEntryNode(N))
            {
                OutLayer.Add(N, 0);
                Frontier.Add(N);
            }
        }

        // If no entries (function graph mid-edit), seed with any exec-bearing nodes.
        if (Frontier.Num() == 0)
        {
            for (UEdGraphNode* N : Graph->Nodes)
            {
                if (N && NodeHasExecPin(N))
                {
                    OutLayer.Add(N, 0);
                    Frontier.Add(N);
                }
            }
        }

        // BFS following outgoing exec links. Layer = max(existing, source+1).
        while (Frontier.Num())
        {
            TArray<UEdGraphNode*> Next;
            for (UEdGraphNode* Src : Frontier)
            {
                const int32 SrcLayer = OutLayer[Src];
                for (UEdGraphPin* P : Src->Pins)
                {
                    if (!IsExecPin(P) || P->Direction != EGPD_Output) continue;
                    for (UEdGraphPin* L : P->LinkedTo)
                    {
                        if (!L || !L->GetOwningNode()) continue;
                        UEdGraphNode* Dst = L->GetOwningNode();
                        int32& DstLayer = OutLayer.FindOrAdd(Dst, INT_MIN);
                        if (SrcLayer + 1 > DstLayer)
                        {
                            DstLayer = SrcLayer + 1;
                            Next.Add(Dst);
                        }
                    }
                }
            }
            Frontier = MoveTemp(Next);
        }
    }

    // Barycenter-based row ordering within a layer. Two passes: forward / backward.
    // Simple average of linked-neighbor row indices in adjacent layers. Good enough
    // to cut most crossings for graphs under ~200 nodes.
    static void OrderRows(TArray<TArray<UEdGraphNode*>>& Layers,
                          const TMap<UEdGraphNode*, int32>& /*LayerOf*/)
    {
        auto Barycenter = [](UEdGraphNode* N, const TMap<UEdGraphNode*, int32>& RowOf) -> float
        {
            int32 Count = 0; float Sum = 0.f;
            for (UEdGraphPin* P : N->Pins)
            {
                if (!IsExecPin(P)) continue;
                for (UEdGraphPin* L : P->LinkedTo)
                {
                    if (!L || !L->GetOwningNode()) continue;
                    if (const int32* R = RowOf.Find(L->GetOwningNode()))
                    {
                        Sum += (float)*R;
                        ++Count;
                    }
                }
            }
            return Count ? Sum / (float)Count : MAX_FLT; // fixed nodes (no links) drift down
        };

        // Seed rows by current array order
        TMap<UEdGraphNode*, int32> RowOf;
        for (const TArray<UEdGraphNode*>& L : Layers)
            for (int32 i = 0; i < L.Num(); ++i)
                RowOf.Add(L[i], i);

        // Forward then backward pass
        for (int32 Pass = 0; Pass < 2; ++Pass)
        {
            for (int32 LayerIdx = 0; LayerIdx < Layers.Num(); ++LayerIdx)
            {
                TArray<UEdGraphNode*>& Layer = Layers[LayerIdx];
                Layer.Sort([&](UEdGraphNode& A, UEdGraphNode& B)
                {
                    return Barycenter(&A, RowOf) < Barycenter(&B, RowOf);
                });
                for (int32 i = 0; i < Layer.Num(); ++i) RowOf.Add(Layer[i], i);
            }
        }
    }

    // Wire-crossing test on straight lines between source/dest node centers.
    // O(N²) across all exec wires — fine for typical graphs (<200 nodes).
    static bool SegmentsCross(const FVector2f& A1, const FVector2f& A2,
                              const FVector2f& B1, const FVector2f& B2)
    {
        auto Ccw = [](const FVector2f& P, const FVector2f& Q, const FVector2f& R)
        {
            return (R.Y - P.Y) * (Q.X - P.X) > (Q.Y - P.Y) * (R.X - P.X);
        };
        return Ccw(A1, B1, B2) != Ccw(A2, B1, B2)
            && Ccw(A1, A2, B1) != Ccw(A1, A2, B2);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
FLinearColor FBlueprintGraphLayout::GetPresetColorForTitle(const FString& Title)
{
    const FString T = Title.ToLower();
    if (T.Contains(TEXT("auth")) || T.Contains(TEXT("server")))
        return FLinearColor(0.2f, 0.8f, 0.4f, 0.3f);
    if (T.Contains(TEXT("rpc")))
        return FLinearColor(0.9f, 0.6f, 0.2f, 0.3f);
    if (T.Contains(TEXT("loop")) || T.Contains(TEXT("for_each")) || T.Contains(TEXT("while")))
        return FLinearColor(0.3f, 0.5f, 0.9f, 0.3f);
    if (T.Contains(TEXT("error")) || T.Contains(TEXT("fail")) || T.Contains(TEXT("invalid")))
        return FLinearColor(0.9f, 0.3f, 0.3f, 0.3f);
    return FLinearColor(0.5f, 0.5f, 0.5f, COMMENT_MIN_ALPHA);
}

void FBlueprintGraphLayout::Layout(UEdGraph* Graph,
                                   const TArray<FBlueprintGraphGroup>& Groups,
                                   bool bInsertReroutes)
{
    if (!Graph) return;

    // Modify() on every exec-touching node for undo support.
    TSet<UEdGraphNode*> Touched;
    for (UEdGraphNode* N : Graph->Nodes) if (N) { N->Modify(); Touched.Add(N); }

    // 1. Layer assignment
    TMap<UEdGraphNode*, int32> LayerOf;
    AssignLayers(Graph, LayerOf);

    // Bucket into layers array
    TArray<TArray<UEdGraphNode*>> Layers;
    for (const auto& KV : LayerOf)
    {
        if (KV.Value < 0) continue;
        while (Layers.Num() <= KV.Value) Layers.AddDefaulted();
        Layers[KV.Value].Add(KV.Key);
    }

    // 2. Row ordering within layer
    OrderRows(Layers, LayerOf);

    // 3. Position per layer. Column X = cumulative max width of previous layers + gaps.
    TMap<UEdGraphNode*, FVector2f> Positions;
    float ColumnX = 0.f;
    for (int32 Li = 0; Li < Layers.Num(); ++Li)
    {
        TArray<UEdGraphNode*>& Layer = Layers[Li];
        float MaxW = 0.f;
        float RowY = 0.f;
        for (int32 Ri = 0; Ri < Layer.Num(); ++Ri)
        {
            UEdGraphNode* N = Layer[Ri];
            const FVector2f Size = EstimateNodeSize(N);
            Positions.Add(N, FVector2f(ColumnX, RowY));
            RowY += Size.Y + GAP_Y;
            if (Size.X > MaxW) MaxW = Size.X;
        }
        ColumnX += MaxW + GAP_X;
    }

    // 4. Attach pure/data-only floaters to their primary exec consumer
    int32 FloaterIdx = 0;
    for (UEdGraphNode* N : Graph->Nodes)
    {
        if (!N || Positions.Contains(N)) continue;
        if (NodeHasExecPin(N)) continue; // exec-bearing but disconnected — leave alone
        // Find the first node that links to one of this floater's output pins
        UEdGraphNode* Consumer = nullptr;
        for (UEdGraphPin* P : N->Pins)
        {
            if (!P) continue;
            if (P->Direction != EGPD_Output) continue;
            for (UEdGraphPin* L : P->LinkedTo)
                if (L && L->GetOwningNode() && Positions.Contains(L->GetOwningNode()))
                    { Consumer = L->GetOwningNode(); break; }
            if (Consumer) break;
        }
        FVector2f Anchor(0.f, 0.f);
        if (Consumer) Anchor = Positions[Consumer];
        Positions.Add(N, FVector2f(Anchor.X - 300.f, Anchor.Y - 150.f - (float)FloaterIdx * 60.f));
        ++FloaterIdx;
    }

    // 5. Apply positions + grid snap
    for (const auto& KV : Positions)
    {
        UEdGraphNode* N = KV.Key;
        N->NodePosX = SnapToGridInt(KV.Value.X);
        N->NodePosY = SnapToGridInt(KV.Value.Y);
    }

    // 6. Insert knots for long or crossing exec wires
    int32 KnotsAdded = 0;
    if (bInsertReroutes)
    {
        // Collect exec wires as (SrcNode, SrcPin, DstPin, DstNode)
        struct FExecWire { UEdGraphNode* S; UEdGraphPin* SP; UEdGraphPin* DP; UEdGraphNode* D; };
        TArray<FExecWire> Wires;
        for (UEdGraphNode* N : Graph->Nodes)
        {
            if (!N) continue;
            for (UEdGraphPin* P : N->Pins)
            {
                if (!IsExecPin(P) || P->Direction != EGPD_Output) continue;
                for (UEdGraphPin* L : P->LinkedTo)
                {
                    if (!L || !L->GetOwningNode()) continue;
                    Wires.Add({ N, P, L, L->GetOwningNode() });
                }
            }
        }

        for (int32 WireIdx = 0; WireIdx < Wires.Num(); ++WireIdx)
        {
            const FExecWire& W = Wires[WireIdx];
            if (!LayerOf.Contains(W.S) || !LayerOf.Contains(W.D)) continue;
            const int32 Span = FMath::Abs(LayerOf[W.D] - LayerOf[W.S]);

            bool bShouldReroute = (Span >= KNOT_MIN_SPAN);
            if (!bShouldReroute)
            {
                // Cheap crossing test against other wires
                const FVector2f A1 = Positions[W.S];
                const FVector2f A2 = Positions[W.D];
                for (int32 Oj = 0; Oj < Wires.Num(); ++Oj)
                {
                    if (Oj == WireIdx) continue;
                    const FExecWire& O = Wires[Oj];
                    if (!Positions.Contains(O.S) || !Positions.Contains(O.D)) continue;
                    if (SegmentsCross(A1, A2, Positions[O.S], Positions[O.D]))
                    { bShouldReroute = true; break; }
                }
            }

            if (!bShouldReroute) continue;

            // Insert a UK2Node_Knot at midpoint
            UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(Graph);
            Graph->AddNode(Knot, false, false);
            Knot->CreateNewGuid();
            Knot->PostPlacedNewNode();
            Knot->AllocateDefaultPins();
            const FVector2f MidPt = (Positions[W.S] + Positions[W.D]) * 0.5f;
            Knot->NodePosX = SnapToGridInt(MidPt.X);
            Knot->NodePosY = SnapToGridInt(MidPt.Y);

            // Rewire: break original link, then reconnect via the schema so
            // NotifyPinConnectionListChanged fires on the knot — that triggers
            // UK2Node_Knot::PropagatePinType internally (the method itself is
            // not BLUEPRINTGRAPH_API exported, so we can't call it directly).
            const UEdGraphSchema* WireSchema = Graph->GetSchema();
            if (WireSchema)
            {
                WireSchema->BreakSinglePinLink(W.SP, W.DP);
                WireSchema->TryCreateConnection(W.SP, Knot->GetInputPin());
                WireSchema->TryCreateConnection(Knot->GetOutputPin(), W.DP);
            }
            else
            {
                // Fallback: raw rewire. Type propagation will still happen on
                // the next NotifyPinConnectionListChanged trigger.
                W.SP->BreakLinkTo(W.DP);
                W.SP->MakeLinkTo(Knot->GetInputPin());
                Knot->GetOutputPin()->MakeLinkTo(W.DP);
            }
            ++KnotsAdded;
        }
    }

    // 7. Emit comment boxes from Groups
    for (const FBlueprintGraphGroup& G : Groups)
    {
        if (G.NodeIds.Num() == 0) continue;

        // Resolve member nodes by GUID-string first, then by name
        TArray<UEdGraphNode*> Members;
        for (const FString& Id : G.NodeIds)
        {
            FGuid Gid;
            UEdGraphNode* Match = nullptr;
            if (FGuid::Parse(Id, Gid))
            {
                for (UEdGraphNode* N : Graph->Nodes) if (N && N->NodeGuid == Gid) { Match = N; break; }
            }
            if (!Match)
            {
                for (UEdGraphNode* N : Graph->Nodes) if (N && N->GetName() == Id) { Match = N; break; }
            }
            if (Match) Members.Add(Match);
        }
        if (Members.Num() == 0) continue;

        // Compute bounding rect in layout space
        float MinX = FLT_MAX, MinY = FLT_MAX, MaxX = -FLT_MAX, MaxY = -FLT_MAX;
        for (UEdGraphNode* M : Members)
        {
            const FVector2f Sz = EstimateNodeSize(M);
            MinX = FMath::Min(MinX, (float)M->NodePosX);
            MinY = FMath::Min(MinY, (float)M->NodePosY);
            MaxX = FMath::Max(MaxX, (float)M->NodePosX + Sz.X);
            MaxY = FMath::Max(MaxY, (float)M->NodePosY + Sz.Y);
        }

        UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(Graph);
        Graph->AddNode(Comment, false, false);
        Comment->CreateNewGuid();
        Comment->PostPlacedNewNode();
        Comment->AllocateDefaultPins();

        Comment->NodePosX    = SnapToGridInt(MinX - COMMENT_PAD);
        Comment->NodePosY    = SnapToGridInt(MinY - COMMENT_PAD - 24.f); // extra top for title
        Comment->NodeWidth   = SnapToGridInt((MaxX - MinX) + 2.f * COMMENT_PAD);
        Comment->NodeHeight  = SnapToGridInt((MaxY - MinY) + 2.f * COMMENT_PAD + 24.f);
        Comment->NodeComment = G.Title;
        Comment->FontSize    = COMMENT_FONT;
        Comment->MoveMode    = ECommentBoxMode::GroupMovement;

        FLinearColor C = G.Color;
        if (C.A < 0.05f) C = FBlueprintGraphLayout::GetPresetColorForTitle(G.Title);
        if (C.A < COMMENT_MIN_ALPHA) C.A = COMMENT_MIN_ALPHA;
        Comment->CommentColor = C;

        for (UEdGraphNode* M : Members) Comment->AddNodeUnderComment(M);
    }

    // 8. Notify editor + mark structurally modified (knots/comments = topology)
    Graph->NotifyGraphChanged();
    if (UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph))
    {
        BP->MarkPackageDirty();
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    }
    (void)KnotsAdded;
}

// ---------------------------------------------------------------------------
// Build FBridgeResult locally — MakeSuccess/MakeError are protected statics on
// UBridgeHandlerBase and out of reach from this free-standing helper.
// ---------------------------------------------------------------------------
namespace
{
    static FBridgeResult MakeLayoutResult(bool bSuccess, const FString& Message,
                                          TSharedPtr<FJsonObject> Data,
                                          int32 ErrorCode = 0,
                                          const FString& RecoveryHint = FString())
    {
        FBridgeResult R;
        R.bSuccess     = bSuccess;
        R.Domain       = TEXT("blueprint");
        R.Action       = TEXT("layout_graph");
        R.Message      = Message;
        R.Data         = Data;
        R.ErrorCode    = ErrorCode;
        R.RecoveryHint = RecoveryHint;
        R.Timestamp    = FDateTime::UtcNow().ToIso8601();
        return R;
    }

    // Best-effort auto-infer of True/False branch subgraphs up to the next exec merge.
    static void InferBranchGroups(UEdGraph* Graph, TArray<FBlueprintGraphGroup>& OutGroups)
    {
        for (UEdGraphNode* N : Graph->Nodes)
        {
            UK2Node_IfThenElse* Br = Cast<UK2Node_IfThenElse>(N);
            if (!Br) continue;

            auto CollectPath = [Graph](UEdGraphPin* FromPin, int32 MaxHops, TArray<FString>& Out)
            {
                if (!FromPin || FromPin->LinkedTo.Num() == 0) return;
                TSet<UEdGraphNode*> Visited;
                TArray<UEdGraphNode*> Frontier;
                for (UEdGraphPin* L : FromPin->LinkedTo)
                    if (L && L->GetOwningNode()) Frontier.Add(L->GetOwningNode());
                int32 Hops = 0;
                while (Frontier.Num() && Hops++ < MaxHops)
                {
                    TArray<UEdGraphNode*> Next;
                    for (UEdGraphNode* Cur : Frontier)
                    {
                        if (!Cur || Visited.Contains(Cur)) continue;
                        Visited.Add(Cur);
                        Out.Add(Cur->NodeGuid.ToString());
                        for (UEdGraphPin* P : Cur->Pins)
                        {
                            if (!P || P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
                            if (P->Direction != EGPD_Output) continue;
                            for (UEdGraphPin* L : P->LinkedTo)
                                if (L && L->GetOwningNode()) Next.Add(L->GetOwningNode());
                        }
                    }
                    Frontier = MoveTemp(Next);
                }
            };

            // "then" == true
            UEdGraphPin* Then  = Br->GetThenPin();
            UEdGraphPin* Else  = Br->GetElsePin();

            if (Then && Then->LinkedTo.Num())
            {
                FBlueprintGraphGroup G; G.Title = TEXT("True path");
                CollectPath(Then, 6, G.NodeIds);
                if (G.NodeIds.Num() >= 2) OutGroups.Add(MoveTemp(G));
            }
            if (Else && Else->LinkedTo.Num())
            {
                FBlueprintGraphGroup G; G.Title = TEXT("False path");
                CollectPath(Else, 6, G.NodeIds);
                if (G.NodeIds.Num() >= 2) OutGroups.Add(MoveTemp(G));
            }
        }
    }

    static void ParseGroupsJson(TSharedPtr<FJsonObject> Params, TArray<FBlueprintGraphGroup>& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* GroupsArr = nullptr;
        if (!Params->TryGetArrayField(TEXT("groups"), GroupsArr) || !GroupsArr) return;
        for (const TSharedPtr<FJsonValue>& V : *GroupsArr)
        {
            TSharedPtr<FJsonObject> O = V.IsValid() ? V->AsObject() : nullptr;
            if (!O.IsValid()) continue;

            FBlueprintGraphGroup G;
            O->TryGetStringField(TEXT("title"), G.Title);

            const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
            if (O->TryGetArrayField(TEXT("nodes"), NodesArr) && NodesArr)
                for (const TSharedPtr<FJsonValue>& NV : *NodesArr)
                    if (NV.IsValid()) G.NodeIds.Add(NV->AsString());

            const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
            if (O->TryGetArrayField(TEXT("color"), ColorArr) && ColorArr && ColorArr->Num() >= 3)
            {
                G.Color.R = (float)(*ColorArr)[0]->AsNumber();
                G.Color.G = (float)(*ColorArr)[1]->AsNumber();
                G.Color.B = (float)(*ColorArr)[2]->AsNumber();
                G.Color.A = ColorArr->Num() >= 4 ? (float)(*ColorArr)[3]->AsNumber() : 0.3f;
            }
            Out.Add(MoveTemp(G));
        }
    }
}

FBridgeResult FBlueprintGraphLayout::LayoutAction(UBlueprint* Blueprint,
                                                  UEdGraph* Graph,
                                                  TSharedPtr<FJsonObject> Params)
{
    if (!Graph) return MakeLayoutResult(false, TEXT("No target graph resolved"), nullptr, 3000);

    bool bInsertReroutes = true;
    bool bInferGroups    = false;
    Params->TryGetBoolField(TEXT("insert_reroutes"), bInsertReroutes);
    Params->TryGetBoolField(TEXT("infer_groups"),    bInferGroups);

    TArray<FBlueprintGraphGroup> Groups;
    ParseGroupsJson(Params, Groups);
    if (bInferGroups) InferBranchGroups(Graph, Groups);

    const int32 NodeCountBefore = Graph->Nodes.Num();
    FBlueprintGraphLayout::Layout(Graph, Groups, bInsertReroutes);
    const int32 NodeCountAfter = Graph->Nodes.Num();
    const int32 AddedNodes      = FMath::Max(0, NodeCountAfter - NodeCountBefore);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("touched_count"),  NodeCountBefore);
    Data->SetNumberField(TEXT("nodes_after"),    NodeCountAfter);
    Data->SetNumberField(TEXT("added_count"),    AddedNodes); // knots + comments
    Data->SetNumberField(TEXT("groups_emitted"), Groups.Num());
    Data->SetBoolField  (TEXT("insert_reroutes"),bInsertReroutes);
    Data->SetBoolField  (TEXT("infer_groups"),   bInferGroups);

    return MakeLayoutResult(true,
        FString::Printf(TEXT("Laid out %d node(s); %d group(s); +%d reroute/comment node(s)"),
            NodeCountBefore, Groups.Num(), AddedNodes),
        Data);
}