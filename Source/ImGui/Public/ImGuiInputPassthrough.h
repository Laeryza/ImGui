#pragma once

#ifndef IMGUI_DISABLE

#include "CoreMinimal.h"

struct FKey;

// ImGui オーバーレイ表示中の入力 passthrough 設定 (generic / プロジェクト非依存)。
//
// FImGuiInputProcessor は通常 ImGui がキーボードを欲しい間 (WantCaptureKeyboard) すべての
// キーイベントを横取りする。本機能を有効化すると:
//   - KeyUp は passthrough 有効時、常にゲーム側へ素通しする
//     → hold 型キーの「離下イベントが ImGui に吸われて届かない = 幽霊」を構造的に排除する。
//   - KeyDown は MovementKeyPredicate が true を返すキーのみ、テキスト編集中 (WantTextInput) を
//     除いてゲーム側へ素通しする (= UI 表示中も移動等を継続できる)。
//
// fork は具体的なキー集合 (WASD 等) を一切知らない。素通し対象はプロジェクトが述語で注入する。
namespace ImGuiInputPassthrough
{
	/** passthrough を有効化する。false (既定) では従来どおり ImGui が WantCaptureKeyboard で吸う。 */
	IMGUI_API void SetEnabled(bool bEnabled);

	/**
	 * KeyDown を素通しする (= UI 表示中も操作できる) キーを判定する述語。
	 * 未設定なら KeyDown は素通ししない (KeyUp の素通しは SetEnabled の状態のみで決まる)。
	 */
	IMGUI_API void SetMovementKeyPredicate(TFunction<bool(const FKey&)> Predicate);
}

#endif // #ifndef IMGUI_DISABLE
