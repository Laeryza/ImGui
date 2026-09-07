// Copyright Trinity. All Rights Reserved.

#include "CoreMinimal.h"

#ifndef IMGUI_DISABLE

#include "Misc/AutomationTest.h"

#include "ImGuiContext.h"
#include "SImGuiOverlay.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS

#endif // #ifndef IMGUI_DISABLE
