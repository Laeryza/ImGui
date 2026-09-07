// Copyright Trinity. All Rights Reserved.

#include "CoreMinimal.h"

#ifndef IMGUI_DISABLE

#include "Misc/AutomationTest.h"

#include "ImGuiContext.h"
#include "SImGuiOverlay.h"

#if WITH_ENGINE
#include "Engine/Texture2D.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtr.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS

// 票 3d4f4669 の回帰固定。map 遷移 GC でテクスチャが破棄されたとき、overlay が抱え続ける古い DrawData
// (生 TexID = GC 済み UTexture*) を描画対象から外すことを検証する。破棄経路 (DestroyTexture 末尾) が撃つ
// seam = FImGuiContext::ClearOverlaysDrawData をこのテストが直接呼ぶ。clear の behavior を戻すと
// HasValidDrawData が true のまま残り、このテストが赤くなる (= リバート検知)。
// Slate の Paint は headless で踏めないため ③ (DrawData クリア) 側のみを固定する。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrinityL2ImGuiOverlayDrawDataClearedTest,
	"Trinity.L2.ImGui.OverlayDrawDataClearedOnTextureDestroy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrinityL2ImGuiOverlayDrawDataClearedTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FImGuiContext> Context = FImGuiContext::Create();
	ImGui::FScopedContext ScopedContext(Context);

	FImGuiViewportData* MainViewportData = FImGuiViewportData::GetOrCreate(ImGui::GetMainViewport());
	if (!TestNotNull(TEXT("main viewport data exists"), MainViewportData))
	{
		return false;
	}

	// 実運用では ImGui_CreateWindow が overlay を配線するが headless では作られないので、
	// main viewport data へ手で貼って破棄経路が overlay へ届くようにする。
	const TSharedRef<SImGuiOverlay> Overlay = SNew(SImGuiOverlay).Context(Context).HandleInput(false);
	MainViewportData->Overlay = Overlay;

	// Render() 済み相当の有効な DrawData を持たせる (実際のクラッシュはこの状態で GC が走ると起きる)。
	ImDrawData DrawData;
	DrawData.Valid = true;
	Overlay->SetDrawData(&DrawData);
	TestTrue(TEXT("overlay holds valid draw data before texture destroy"), Overlay->HasValidDrawData());

	// テクスチャ破棄経路 (DestroyTexture) が末尾で撃つ seam。
	Context->ClearOverlaysDrawData();

	TestFalse(TEXT("overlay draw data cleared after texture destroy"), Overlay->HasValidDrawData());

	return true;
}

#if WITH_ENGINE

// 票 3d4f4669 の本丸。overlay が抱える DrawData の TexID は生の UTexture* なので、参照先を DrawData 自身が
// 強参照で押さえていないと map 遷移の GC でテクスチャが消え、次の OnPaint が解放済みポインタを踏む。
// IsValid() は非 null の dangling を弾けない (中で触って落ちる) ため、寿命側で塞ぐしかない。
// 実際に GC を回し「DrawData が生きている間はテクスチャも生きる / 手放したら回収される」を両側から固定する。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTrinityL2ImGuiDrawDataKeepsTextureAliveTest,
	"Trinity.L2.ImGui.DrawDataKeepsReferencedTextureAliveAcrossGC",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FTrinityL2ImGuiDrawDataKeepsTextureAliveTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FImGuiContext> Context = FImGuiContext::Create();
	ImGui::FScopedContext ScopedContext(Context);

	const TSharedRef<SImGuiOverlay> Overlay = SNew(SImGuiOverlay).Context(Context).HandleInput(false);

	// DrawData 以外に強参照を持たせない = GC の対象になりうる状態のテクスチャを 1 枚作る。
	TWeakObjectPtr<UTexture2D> WeakTexture;
	{
		UTexture2D* Texture = UTexture2D::CreateTransient(4, 4, PF_B8G8R8A8);
		WeakTexture = Texture;

		// draw cmd 1 本だけの draw list を組んで overlay に渡す (ImGui の Render() 出力相当)。
		ImDrawList DrawList(ImGui::GetDrawListSharedData());
		ImDrawCmd DrawCmd;
		DrawCmd.TexRef = ImTextureRef(Texture);
		DrawList.CmdBuffer.push_back(DrawCmd);

		ImDrawList* DrawListPtr = &DrawList;
		ImDrawData DrawData;
		DrawData.Valid = true;
		DrawData.CmdLists.push_back(DrawListPtr);

		Overlay->SetDrawData(&DrawData);

		// ImDrawData のデストラクタに CmdLists の所有権を渡さない (スタック上の DrawList を指しているだけ)。
		DrawData.CmdLists.clear();
	}

	CollectGarbage(RF_NoFlags, true);
	TestTrue(TEXT("DrawData が参照するテクスチャは GC を跨いで生き残る"), WeakTexture.IsValid());

	// 逆側: DrawData を手放せば強参照も消え、テクスチャは回収される (pin しっぱなしのリークでないこと)。
	Overlay->ClearDrawData();
	CollectGarbage(RF_NoFlags, true);
	TestFalse(TEXT("DrawData を捨てたテクスチャは GC で回収される"), WeakTexture.IsValid());

	return true;
}

#endif // WITH_ENGINE

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // #ifndef IMGUI_DISABLE
