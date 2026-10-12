// In-game menu: hooks DXGI Present (D3D11 + D3D12) and draws an ImGui window toggled with a hotkey.
#pragma once

namespace overlay {
void start(unsigned toggleKey);  // spawns its own thread; safe to call before the engine is ready
}
