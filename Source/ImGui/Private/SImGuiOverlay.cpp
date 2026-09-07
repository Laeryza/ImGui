#include "SImGuiOverlay.h"

#ifndef IMGUI_DISABLE

#include <Framework/Application/SlateApplication.h>

#include "ImGuiContext.h"
#include "ImGuiInputPassthrough.h"

FImGuiDrawList::FImGuiDrawList(ImDrawList* Source)
{
	VtxBuffer.swap(Source->VtxBuffer);
	IdxBuffer.swap(Source->IdxBuffer);
	CmdBuffer.swap(Source->CmdBuffer);
	Flags = Source->Flags;
}

FImGuiDrawData::FImGuiDrawData(const ImDrawData* Source)
{
	bValid = Source->Valid;

	TotalIdxCount = Source->TotalIdxCount;
	TotalVtxCount = Source->TotalVtxCount;

	ImGui::CopyArray(Source->CmdLists, DrawLists);

	DisplayPos = Source->DisplayPos;
	DisplaySize = Source->DisplaySize;
	FrameBufferScale = Source->FramebufferScale;
}

// 入力 passthrough の経路決定ログ用カテゴリ。既定 verbosity では無音で、
// 症状発生時にコンソールで `Log LogImGuiInput Verbose` を打つとフレーム単位の裁定履歴が取れる。
DEFINE_LOG_CATEGORY_STATIC(LogImGuiInput, Log, All);

namespace ImGuiInputPassthrough
{
	// FImGuiInputProcessor が参照する設定 (本 TU 内 file-local)。
	// 外部からは公開 API (ImGuiInputPassthrough.h) 経由で設定する。
	static bool bEnabled = false;
	static TFunction<bool(const FKey&)> MovementKeyPredicate;

	void SetEnabled(bool bInEnabled)
	{
		bEnabled = bInEnabled;
	}

	void SetMovementKeyPredicate(TFunction<bool(const FKey&)> Predicate)
	{
		MovementKeyPredicate = MoveTemp(Predicate);
	}

	bool IsEnabled()
	{
		return bEnabled;
	}

	bool HasMovementKeyPredicate()
	{
		return static_cast<bool>(MovementKeyPredicate);
	}
}

class FImGuiInputProcessor : public IInputProcessor
{
public:
	explicit FImGuiInputProcessor(SImGuiOverlay* InOwner)
	{
		Owner = InOwner;

		FSlateApplication::Get().OnApplicationActivationStateChanged().AddRaw(this, &FImGuiInputProcessor::OnApplicationActivationChanged);
		FSlateApplication::Get().OnFocusChanging().AddRaw(this, &FImGuiInputProcessor::OnFocusChanging);

		LastFocusedWindow = FSlateApplication::Get().GetActiveTopLevelRegularWindow();
	}

	virtual ~FImGuiInputProcessor() override
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().OnApplicationActivationStateChanged().RemoveAll(this);
			FSlateApplication::Get().OnFocusChanging().RemoveAll(this);
		}
	}

	void OnApplicationActivationChanged(bool bIsActive) const
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		ImGuiIO& IO = ImGui::GetIO();

		IO.AddFocusEvent(bIsActive);
	}

	void OnFocusChanging(const FFocusEvent& Event, const FWeakWidgetPath& OldWidgetPath, const TSharedPtr<SWidget>& OldWidget, const FWidgetPath& NewWidgetPath, const TSharedPtr<SWidget>& NewWidget)
	{
		if (NewWidgetPath.IsValid())
		{
			LastFocusedWindow = NewWidgetPath.GetDeepestWindow();
		}
		else
		{
			LastFocusedWindow.Reset();
		}
	}

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> SlateCursor) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		ImGuiIO& IO = ImGui::GetIO();

		const bool bHasGamepad = (IO.BackendFlags & ImGuiBackendFlags_HasGamepad);
		if (bHasGamepad != SlateApp.IsGamepadAttached())
		{
			IO.BackendFlags ^= ImGuiBackendFlags_HasGamepad;
		}

		if (IO.WantSetMousePos)
		{
			FVector2f Position = IO.MousePos;
			if (!(IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
			{
				// Mouse position for single viewport mode is in client space
				Position += Owner->GetTickSpaceGeometry().AbsolutePosition;
			}

			SlateCursor->SetPosition(Position.X, Position.Y);
		}

		if (IO.WantTextInput && !Owner->HasKeyboardFocus())
		{
			// No HandleKeyCharEvent so punt focus to the widget for it to receive OnKeyChar events
			SlateApp.SetKeyboardFocus(Owner->AsShared());
		}
	}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		IO.AddKeyEvent(ImGui::ConvertKey(Event.GetKey()), true);

		const FModifierKeysState& ModifierKeys = Event.GetModifierKeys();
		IO.AddKeyEvent(ImGuiMod_Ctrl, ModifierKeys.IsControlDown());
		IO.AddKeyEvent(ImGuiMod_Shift, ModifierKeys.IsShiftDown());
		IO.AddKeyEvent(ImGuiMod_Alt, ModifierKeys.IsAltDown());
		IO.AddKeyEvent(ImGuiMod_Super, ModifierKeys.IsCommandDown());

		// 移動キー passthrough: 述語が「素通し対象」と判定したキーは、テキスト編集中 (WantTextInput) を
		// 除いてゲーム側へ素通しする (UI 表示中も移動継続)。述語は DOWN 素通し対象 = UI 中に動けるキーの
		// 決定のみを担う。集合が多少不完全でも「そのキーで UI 中に動けない」程度で幽霊にはならない。
		const bool bPassthroughEnabled = ImGuiInputPassthrough::bEnabled;
		const bool bHasPredicate = static_cast<bool>(ImGuiInputPassthrough::MovementKeyPredicate);
		const bool bPredicateHit = bPassthroughEnabled && bHasPredicate
			&& ImGuiInputPassthrough::MovementKeyPredicate(Event.GetKey());

		// 裁定: true を返すと ImGui が消費 (ゲームに届かない)、false ならゲーム側へ素通し。
		const bool bConsumedByImGui = bPredicateHit ? IO.WantTextInput : IO.WantCaptureKeyboard;

		// 経路決定ログ (verbose 限定・既定では無音)。WASD 間欠死の再発時に、どの分岐で
		// 消費されたかをフレーム単位で辿るための常設計装。
		UE_LOG(LogImGuiInput, Verbose,
			TEXT("KeyDown '%s': enabled=%d predicate=%d hit=%d WantTextInput=%d WantCaptureKeyboard=%d -> %s"),
			*Event.GetKey().ToString(),
			bPassthroughEnabled ? 1 : 0, bHasPredicate ? 1 : 0, bPredicateHit ? 1 : 0,
			IO.WantTextInput ? 1 : 0, IO.WantCaptureKeyboard ? 1 : 0,
			bConsumedByImGui ? TEXT("ImGui が消費") : TEXT("ゲームへ素通し"));

		return bConsumedByImGui;
	}

	virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& Event) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		IO.AddKeyEvent(ImGui::ConvertKey(Event.GetKey()), false);

		const FModifierKeysState& ModifierKeys = Event.GetModifierKeys();
		IO.AddKeyEvent(ImGuiMod_Ctrl, ModifierKeys.IsControlDown());
		IO.AddKeyEvent(ImGuiMod_Shift, ModifierKeys.IsShiftDown());
		IO.AddKeyEvent(ImGuiMod_Alt, ModifierKeys.IsAltDown());
		IO.AddKeyEvent(ImGuiMod_Super, ModifierKeys.IsCommandDown());

		// passthrough 有効時は全キーの離下を常にゲーム側へ素通しする。
		// hold 型キーの「離下が ImGui に吸われて届かない = 幽霊」を構造的に排除する要 (flush 不要化の核心)。
		if (ImGuiInputPassthrough::bEnabled)
		{
			return false;
		}
		return IO.WantCaptureKeyboard;
	}

	virtual bool HandleAnalogInputEvent(FSlateApplication& SlateApp, const FAnalogInputEvent& Event) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		const float Value = Event.GetAnalogValue();
		IO.AddKeyAnalogEvent(ImGui::ConvertKey(Event.GetKey()), FMath::Abs(Value) > 0.1f, Value);

		return IO.WantCaptureKeyboard;
	}

	virtual bool HandleMouseMoveEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		const TSharedPtr<FSlateUser> SlateUser = SlateApp.GetUser(Event.GetUserIndex());
		if (SlateUser.IsValid())
		{
			const FImGuiViewportData* TargetViewport = nullptr;

			if (!SlateUser->HasCapture(Event.GetPointerIndex()))
			{
				const FWeakWidgetPath LastWidgetsUnderPointer = SlateUser->GetLastWidgetsUnderPointer(Event.GetPointerIndex());
				TargetViewport = FindViewportForWindow(LastWidgetsUnderPointer.Window.Pin());
			}

			if (!TargetViewport && !ImGui::IsMouseDown(0))
			{
				IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
				return false;
			}
		}

		FVector2f Position = Event.GetScreenSpacePosition();
		if (!(IO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
		{
			// Mouse position for single viewport mode is in client space
			Position -= Owner->GetTickSpaceGeometry().AbsolutePosition;
		}

		IO.AddMousePosEvent(Position.X, Position.Y);

		return IO.WantCaptureMouse;
	}

	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		const FKey Button = Event.GetEffectingButton();
		if (Button == EKeys::LeftMouseButton)
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
		}
		else if (Button == EKeys::RightMouseButton)
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
		}
		else if (Button == EKeys::MiddleMouseButton)
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
		}

		return IO.WantCaptureMouse;
	}

	virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		const FKey Button = Event.GetEffectingButton();
		if (Button == EKeys::LeftMouseButton)
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		}
		else if (Button == EKeys::RightMouseButton)
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
		}
		else if (Button == EKeys::MiddleMouseButton)
		{
			IO.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
		}

		return false;
	}

	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
	{
		// Treat as mouse down, ImGui handles double click internally
		return HandleMouseButtonDownEvent(SlateApp, Event);
	}

	virtual bool HandleMouseWheelOrGestureEvent(FSlateApplication& SlateApp, const FPointerEvent& Event, const FPointerEvent* GestureEvent) override
	{
		ImGui::FScopedContext ScopedContext(Owner->GetContext());

		if (!ShouldHandleEvent(SlateApp, Event))
		{
			return false;
		}

		ImGuiIO& IO = ImGui::GetIO();

		IO.AddMouseWheelEvent(0.0f, Event.GetWheelDelta());

		return IO.WantCaptureMouse;
	}

	bool ShouldHandleEvent(FSlateApplication& SlateApp, const FInputEvent& Event) const
	{
#if WITH_EDITORONLY_DATA
		if (GIntraFrameDebuggingGameThread)
		{
			// Discard input events when the game thread is paused for debugging
			return false;
		}
#endif

		if (Event.IsKeyEvent())
		{
			const FImGuiViewportData* FocusedViewport = FindViewportForWindow(LastFocusedWindow.Pin());
			return FocusedViewport != nullptr;
		}

		return true;
	}

	static FImGuiViewportData* FindViewportForWindow(const TSharedPtr<SWindow>& Window)
	{
		if (!Window.IsValid())
		{
			return nullptr;
		}

		for (ImGuiViewport* Viewport : ImGui::GetPlatformIO().Viewports)
		{
			FImGuiViewportData* ViewportData = FImGuiViewportData::GetOrCreate(Viewport);
			if (ViewportData->Window == Window)
			{
				return ViewportData;
			}
		}

		return nullptr;
	}

private:
	SImGuiOverlay* Owner = nullptr;
	TWeakPtr<SWindow> LastFocusedWindow;
};

void SImGuiOverlay::Construct(const FArguments& Args)
{
	SetVisibility(EVisibility::HitTestInvisible);
	ForceVolatile(true);

	Context = Args._Context.IsValid() ? Args._Context : FImGuiContext::Create();
	if (Args._HandleInput)
	{
		InputProcessor = MakeShared<FImGuiInputProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor.ToSharedRef(), 0);
	}
}

SImGuiOverlay::~SImGuiOverlay()
{
	if (FSlateApplication::IsInitialized() && InputProcessor.IsValid())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
	}
}

int32 SImGuiOverlay::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	if (!DrawData.bValid)
	{
		return LayerId;
	}

	const FSlateRenderTransform Transform(AllottedGeometry.GetAccumulatedRenderTransform().GetTranslation() - FVector2d(DrawData.DisplayPos));

	TArray<FSlateVertex> Vertices;
	TArray<SlateIndex> Indices;
	FSlateBrush TextureBrush;

	for (const FImGuiDrawList& DrawList : DrawData.DrawLists)
	{
		Vertices.SetNumUninitialized(DrawList.VtxBuffer.Size);

		ImDrawVert* SrcVertex = DrawList.VtxBuffer.Data;
		FSlateVertex* DstVertex = Vertices.GetData();

		for (int32 BufferIdx = 0; BufferIdx < Vertices.Num(); ++BufferIdx, ++SrcVertex, ++DstVertex)
		{
			DstVertex->TexCoords[0] = SrcVertex->uv.x;
			DstVertex->TexCoords[1] = SrcVertex->uv.y;
			DstVertex->TexCoords[2] = 1;
			DstVertex->TexCoords[3] = 1;
			DstVertex->Position = TransformPoint(Transform, FVector2f(SrcVertex->pos));
			DstVertex->Color.Bits = SrcVertex->col;
		}

		ImGui::CopyArray(DrawList.IdxBuffer, Indices);

		for (const ImDrawCmd& DrawCmd : DrawList.CmdBuffer)
		{
#if WITH_ENGINE
			UTexture* Texture = DrawCmd.GetTexID();
			// GC 済みテクスチャの生ポインタを SetResourceObject に渡すと AV (null+0x38)。map 遷移 GC 後の
			// 古い DrawData への保険として、無効な TexID の DrawCmd は描画ごと skip する (以降は有効前提)。
			if (!IsValid(Texture))
			{
				continue;
			}
			if (TextureBrush.GetResourceObject() != Texture)
			{
				TextureBrush.SetResourceObject(Texture);
				TextureBrush.ImageSize.X = Texture->GetSurfaceWidth();
				TextureBrush.ImageSize.Y = Texture->GetSurfaceHeight();
				TextureBrush.ImageType = ESlateBrushImageType::FullColor;
				TextureBrush.DrawAs = ESlateBrushDrawType::Image;
			}
#else
			FSlateBrush* Texture = DrawCmd.GetTexID();
			if (Texture)
			{
				TextureBrush = *Texture;
			}
			else
			{
				TextureBrush.ImageSize.X = 0;
				TextureBrush.ImageSize.Y = 0;
				TextureBrush.ImageType = ESlateBrushImageType::NoImage;
				TextureBrush.DrawAs = ESlateBrushDrawType::NoDrawType;
			}
#endif

			FSlateRect ClipRect(DrawCmd.ClipRect.x, DrawCmd.ClipRect.y, DrawCmd.ClipRect.z, DrawCmd.ClipRect.w);
			ClipRect = TransformRect(Transform, ClipRect);

			OutDrawElements.PushClip(FSlateClippingZone(ClipRect));

			FSlateDrawElement::MakeCustomVerts(
				OutDrawElements, LayerId, TextureBrush.GetRenderingResource(),
				TArray(Vertices.GetData() + DrawCmd.VtxOffset, Vertices.Num() - DrawCmd.VtxOffset),
				TArray(Indices.GetData() + DrawCmd.IdxOffset, DrawCmd.ElemCount),
				nullptr, 0, 0
			);

			OutDrawElements.PopClip();
		}
	}

	return LayerId;
}

FVector2D SImGuiOverlay::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D::ZeroVector;
}

bool SImGuiOverlay::SupportsKeyboardFocus() const
{
	return true;
}

FReply SImGuiOverlay::OnKeyChar(const FGeometry& MyGeometry, const FCharacterEvent& Event)
{
	ImGui::FScopedContext ScopedContext(Context);

	ImGuiIO& IO = ImGui::GetIO();

	IO.AddInputCharacter(Event.GetCharacter());

	return IO.WantTextInput ? FReply::Handled() : FReply::Unhandled();
}

TSharedPtr<FImGuiContext> SImGuiOverlay::GetContext() const
{
	return Context;
}

void SImGuiOverlay::SetDrawData(const ImDrawData* InDrawData)
{
	DrawData = FImGuiDrawData(InDrawData);
}

void SImGuiOverlay::ClearDrawData()
{
	DrawData = FImGuiDrawData();
}

bool SImGuiOverlay::HasValidDrawData() const
{
	return DrawData.bValid;
}

#endif // #ifndef IMGUI_DISABLE
