/**
 * @file        ui/overlay/lobby_panel.cpp
 * @brief       Game-driven lobby panel (see rex/ui/overlay/lobby_panel.h).
 *
 * Draws like the game's own main menu: rows are bars at the stock rows'
 * positions (measured on the 1280x720 main menu: x 232..618, 54 px tall,
 * 58 px apart, from y 110), with a dark leather fill, a copper rim and a
 * rounded right end; the selected row has the green A medallion on its left.
 * Text uses the game's bitmap font (.bft) read from the player's own data:
 * 22-byte glyph records, a U+0020.. character map and a 4 bpp BC4 atlas stored
 * Xbox 360 tiled and 16-bit byte-swapped (Fable-2-Recomp tools/bft_font.py).
 * Nothing of the game's art is shipped; shapes and colours are drawn here.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/overlay/lobby_panel.h>
#include <imgui.h>
#include <rex/logging/macros.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace rex::ui {

namespace {

std::mutex g_view_mutex;
std::string g_view;
std::string g_cover;
std::string g_font_path;
// The toast (rex_ui_set_toast): text under g_view_mutex; until = steady-clock ms
// it hides at, 0 = none (the presenter reads only this while no toast is up).
std::string g_toast;
std::atomic<int64_t> g_toast_until_ms{0};

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// --- the game's bitmap font -------------------------------------------------------

struct Glyph {
  float u0, v0, u1, v1;
  int16_t xoff, width, advance;
};

struct BftFont {
  bool loaded = false;
  int atlas_w = 0, atlas_h = 0;
  float line_h = 0;
  std::unordered_map<uint32_t, Glyph> glyphs;
  std::vector<uint8_t> rgba;  // white, alpha = coverage
};

BftFont g_font;
std::string g_font_loaded_path;

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
float bef(const uint8_t* p) {
  const uint32_t u = be32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// XGAddress2DTiledOffset (Xbox 360), in blocks.
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t width_blocks, uint32_t log2_bpb) {
  const uint32_t aligned_w = (width_blocks + 31) & ~31u;
  const uint32_t macro = ((y >> 5) * (aligned_w >> 5) + (x >> 5)) << (log2_bpb + 7);
  const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpb;
  const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return (((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
          (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F)) >>
         log2_bpb;
}

bool LoadBft(const std::string& path, BftFont& f) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  const std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
  if (d.size() < 64) return false;
  f.line_h = float(be32(&d[4]));
  const uint32_t count = be16(&d[23]);
  if (25 + size_t(count) * 22 + 68 > d.size()) return false;
  std::vector<Glyph> recs(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* p = &d[25 + i * 22];
    recs[i] = {bef(p), bef(p + 4), bef(p + 8), bef(p + 12), int16_t(be16(p + 16)),
               int16_t(be16(p + 18)), int16_t(be16(p + 20))};
  }
  size_t i = 25 + size_t(count) * 22 + 4 + 64;
  uint32_t ch = 0x20;
  for (; i + 2 <= d.size(); i += 2, ++ch) {
    const uint16_t v = be16(&d[i]);
    if (v != 0x3434 && v >= count) break;
    if (v != 0x3434) f.glyphs[ch] = recs[v];
  }
  if (i + 24 > d.size()) return false;
  const uint32_t w = be32(&d[i + 16]), h = be32(&d[i + 20]);
  if (!w || !h || w > 4096 || h > 4096 || size_t(w) * h / 2 > d.size()) return false;
  std::vector<uint8_t> tex(d.end() - size_t(w) * h / 2, d.end());
  for (size_t k = 0; k + 1 < tex.size(); k += 2) std::swap(tex[k], tex[k + 1]);
  f.atlas_w = int(w);
  f.atlas_h = int(h);
  f.rgba.assign(size_t(w) * h * 4, 255);
  const uint32_t bw = w / 4, bh = h / 4;
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const size_t idx = TiledOffset(bx, by, bw, 3);
      if (idx * 8 + 8 > tex.size()) continue;
      const uint8_t* b = &tex[idx * 8];
      uint8_t pal[8] = {b[0], b[1]};
      if (b[0] > b[1]) {
        for (int k = 1; k <= 6; ++k) pal[k + 1] = uint8_t(((7 - k) * b[0] + k * b[1]) / 7);
      } else {
        for (int k = 1; k <= 4; ++k) pal[k + 1] = uint8_t(((5 - k) * b[0] + k * b[1]) / 5);
        pal[6] = 0;
        pal[7] = 255;
      }
      uint64_t bits = 0;
      for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
      for (int j = 0; j < 16; ++j) {
        const uint32_t px = bx * 4 + j % 4, py = by * 4 + j / 4;
        f.rgba[(size_t(py) * w + px) * 4 + 3] = pal[(bits >> (3 * j)) & 7];
      }
    }
  }
  f.loaded = !f.glyphs.empty();
  return f.loaded;
}

// --- drawing ------------------------------------------------------------------------

// Colours sampled from the stock main menu.
const ImU32 kGold = IM_COL32(234, 210, 126, 255);       // row text
const ImU32 kParchment = IM_COL32(222, 206, 170, 255);  // plain text
const ImU32 kDim = IM_COL32(150, 132, 100, 255);
const ImU32 kBarFill = IM_COL32(52, 38, 22, 255);
const ImU32 kBarHi = IM_COL32(78, 60, 36, 120);
const ImU32 kCopper = IM_COL32(196, 128, 84, 255);
const ImU32 kCopperDark = IM_COL32(92, 52, 30, 255);
const ImU32 kLeather = IM_COL32(44, 33, 20, 255);

uint32_t NextCodepoint(const std::string& s, size_t& i) {
  const uint8_t c = uint8_t(s[i++]);
  if (c < 0x80) return c;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
  uint32_t cp = c & (0x3F >> extra);
  while (extra-- > 0 && i < s.size()) cp = (cp << 6) | (uint8_t(s[i++]) & 0x3F);
  return cp;
}

float TextWidth(const std::string& s, float scale) {
  if (!g_font.loaded) return ImGui::CalcTextSize(s.c_str()).x * scale;
  float w = 0;
  for (size_t i = 0; i < s.size();) {
    auto it = g_font.glyphs.find(NextCodepoint(s, i));
    if (it != g_font.glyphs.end()) w += it->second.advance * scale;
  }
  return w;
}

void DrawText(ImDrawList* dl, ImTextureID tex, ImVec2 pos, float scale, ImU32 col,
              const std::string& s) {
  if (!g_font.loaded || !tex) {
    dl->AddText(nullptr, ImGui::GetFontSize() * scale * 1.6f, pos, col, s.c_str());
    return;
  }
  float x = pos.x;
  for (size_t i = 0; i < s.size();) {
    auto it = g_font.glyphs.find(NextCodepoint(s, i));
    if (it == g_font.glyphs.end()) continue;
    const Glyph& g = it->second;
    const float gw = (g.u1 - g.u0) * g_font.atlas_w * scale;
    const float gh = (g.v1 - g.v0) * g_font.atlas_h * scale;
    const ImVec2 a(x + g.xoff * scale, pos.y);
    dl->AddImage(tex, a, ImVec2(a.x + gw, a.y + gh), ImVec2(g.u0, g.v0), ImVec2(g.u1, g.v1), col);
    x += g.advance * scale;
  }
}

void DrawBar(ImDrawList* dl, ImVec2 a, ImVec2 b, float s, bool dim) {
  const float r = (b.y - a.y) * 0.5f;
  dl->AddRectFilled(ImVec2(a.x + 2 * s, a.y + 3 * s), ImVec2(b.x + 2 * s, b.y + 3 * s),
                    IM_COL32(0, 0, 0, 110), r, ImDrawFlags_RoundCornersRight);  // shadow
  dl->AddRectFilled(a, b, dim ? IM_COL32(40, 30, 18, 255) : kBarFill, r, ImDrawFlags_RoundCornersRight);
  dl->AddRectFilled(ImVec2(a.x, a.y + 3 * s), ImVec2(b.x - r * 0.6f, a.y + (b.y - a.y) * 0.45f), kBarHi,
                    r * 0.5f, ImDrawFlags_RoundCornersRight);
  dl->AddRect(a, b, kCopperDark, r, ImDrawFlags_RoundCornersRight, 5 * s);
  dl->AddRect(ImVec2(a.x, a.y + 1 * s), ImVec2(b.x - 1 * s, b.y - 1 * s), kCopper, r,
              ImDrawFlags_RoundCornersRight, 2.2f * s);
}

void DrawMedallion(ImDrawList* dl, ImTextureID tex, ImVec2 c, float s) {
  const float r = 26 * s;
  dl->AddCircleFilled(ImVec2(c.x + 2 * s, c.y + 3 * s), r, IM_COL32(0, 0, 0, 120), 40);
  dl->AddCircleFilled(c, r, kCopperDark, 40);
  dl->AddCircleFilled(c, r - 2.5f * s, kCopper, 40);
  dl->AddCircleFilled(c, r - 6 * s, IM_COL32(34, 92, 24, 255), 40);
  dl->AddCircleFilled(ImVec2(c.x - 1.5f * s, c.y - 2 * s), r - 9 * s, IM_COL32(82, 168, 48, 255), 40);
  dl->AddCircleFilled(ImVec2(c.x - 4 * s, c.y - 7 * s), r - 17 * s, IM_COL32(150, 214, 110, 110), 32);
  const float ts = 0.95f * s;
  const float tw = TextWidth("A", ts);
  DrawText(dl, tex, ImVec2(c.x - tw * 0.5f, c.y - g_font.line_h * ts * 0.52f), ts,
           IM_COL32(250, 250, 245, 255), "A");
}

struct Line {
  char kind;
  bool enabled = true, selected = false;
  std::string text;
};

}  // namespace

LobbyPanelDialog::LobbyPanelDialog(ImGuiDrawer* imgui_drawer) : ImGuiDialog(imgui_drawer) {}
LobbyPanelDialog::~LobbyPanelDialog() {}

bool LobbyPanelDialog::EnsureFont() {
  std::string path;
  {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    path = g_font_path;
  }
  if (path != g_font_loaded_path) {
    g_font_loaded_path = path;
    g_font = BftFont{};
    font_texture_.reset();
    if (!path.empty()) {
      const bool ok = LoadBft(path, g_font);
      REXLOG_INFO("[lobby-panel] font {}: {} ({} glyphs, atlas {}x{})", path, ok ? "loaded" : "FAILED",
                  g_font.glyphs.size(), g_font.atlas_w, g_font.atlas_h);
    }
  }
  if (!g_font.loaded) return false;
  ImmediateDrawer* drawer = imgui_drawer()->immediate_drawer();
  if (!drawer) return false;
  if (!font_texture_ || font_drawer_ != drawer) {
    font_texture_ = drawer->CreateTexture(uint32_t(g_font.atlas_w), uint32_t(g_font.atlas_h),
                                          ImmediateTextureFilter::kLinear, false, g_font.rgba.data());
    font_drawer_ = drawer;
  }
  return font_texture_ != nullptr;
}

void LobbyPanelDialog::DrawToast(ImGuiIO& io) {
  int64_t until = g_toast_until_ms.load(std::memory_order_relaxed);
  if (!until) return;
  const int64_t left = until - SteadyMs();
  if (left <= 0) {
    g_toast_until_ms.compare_exchange_strong(until, 0);
    return;
  }
  std::string text;
  {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    if (!g_cover.empty()) return;  // the cover hides everything anyway
    text = g_toast;
  }
  if (text.empty()) return;

  const ImTextureID tex =
      EnsureFont() ? reinterpret_cast<ImTextureID>(font_texture_.get()) : ImTextureID{};
  const float W = io.DisplaySize.x, H = io.DisplaySize.y;
  const float s = std::min(W / 1280.0f, H / 720.0f);
  const float lh = (g_font.loaded ? g_font.line_h : 32.0f);
  const float ts = 0.66f * s;
  // Fades out over its last 300 ms.
  const float fade = std::min(1.0f, float(left) / 300.0f);
  auto alpha = [fade](ImU32 c) {
    return (c & ~IM_COL32_A_MASK) | (ImU32(((c >> IM_COL32_A_SHIFT) & 0xFF) * fade) << IM_COL32_A_SHIFT);
  };
  // A small pill at the bottom centre, clear of the game's own HUD corners.
  const float tw = TextWidth(text, ts), pad = 18 * s, ph = lh * ts + 14 * s;
  const ImVec2 a(W * 0.5f - tw * 0.5f - pad, H * 0.80f), b(W * 0.5f + tw * 0.5f + pad, H * 0.80f + ph);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(a.x + 2 * s, a.y + 3 * s), ImVec2(b.x + 2 * s, b.y + 3 * s),
                    alpha(IM_COL32(0, 0, 0, 110)), ph * 0.5f);
  dl->AddRectFilled(a, b, alpha(kBarFill), ph * 0.5f);
  dl->AddRect(a, b, alpha(kCopper), ph * 0.5f, 0, 2.0f * s);
  DrawText(dl, tex, ImVec2(a.x + pad, a.y + 7 * s), ts, alpha(kGold), text);
}

void LobbyPanelDialog::OnDraw(ImGuiIO& io) {
  DrawToast(io);  // independent of the view and drawn even when it is empty
  std::string view;
  {
    std::lock_guard<std::mutex> lock(g_view_mutex);
    // The cover wins over the view (and is set separately, so a menu closing
    // underneath it cannot clear it).
    if (!g_cover.empty()) {
      std::string c = "C";
      for (char ch : g_cover) {
        c.push_back(ch);
        if (ch == '\n') c.push_back('D');
      }
      view = c;
    } else {
      view = g_view;
    }
  }
  static size_t s_logged_size = ~size_t(0);
  if (view.size() != s_logged_size) {
    s_logged_size = view.size();
    REXLOG_INFO("[lobby-panel] draw: view {} bytes, display {}x{}", view.size(), io.DisplaySize.x,
                io.DisplaySize.y);
  }
  if (view.empty()) return;

  const ImTextureID tex =
      EnsureFont() ? reinterpret_cast<ImTextureID>(font_texture_.get()) : ImTextureID{};
  std::vector<Line> lines;
  std::string title, cover;
  {
    std::istringstream in(view);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty()) continue;
      Line l{line[0]};
      l.text = line.substr(1);
      if (l.kind == 'T') title = l.text;
      else if (l.kind == 'C') cover = l.text;
      else if (l.kind == 'R' && l.text.size() >= 2) {
        l.enabled = l.text[0] == '1';
        l.selected = l.text[1] == '1';
        l.text = l.text.substr(2);
        lines.push_back(l);
      } else {
        lines.push_back(l);
      }
    }
  }

  const float W = io.DisplaySize.x, H = io.DisplaySize.y;
  const float sx = W / 1280.0f, sy = H / 720.0f, s = std::min(sx, sy);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float row_scale = 0.91f * s;  // gui_large at the stock rows' size
  const float lh = (g_font.loaded ? g_font.line_h : 32.0f);

  if (!cover.empty()) {
    // Full-screen card: nothing of the game shows through.
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), kLeather);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H * 0.5f), IM_COL32(0, 0, 0, 90),
                                IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(ImVec2(0, H * 0.5f), ImVec2(W, H), IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 110), IM_COL32(0, 0, 0, 110));
    const float bw = 620 * sx, bh = 54 * sy;
    const ImVec2 ba(W * 0.5f - bw * 0.5f, H * 0.5f - bh * 0.5f);
    DrawBar(dl, ba, ImVec2(ba.x + bw, ba.y + bh), s, false);
    const float ts = row_scale * 1.05f;
    DrawText(dl, tex, ImVec2(W * 0.5f - TextWidth(cover, ts) * 0.5f, ba.y + (bh - lh * ts) * 0.5f), ts,
             kGold, cover);
    // A slow pulse so the card never looks frozen.
    const float t = float(ImGui::GetTime());
    for (int k = 0; k < 3; ++k) {
      const float a = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(t * 3.0f - k * 0.9f));
      dl->AddCircleFilled(ImVec2(W * 0.5f + (k - 1) * 22 * s, ba.y + bh + 30 * s), 5 * s,
                          IM_COL32(234, 210, 126, int(255 * a)), 16);
    }
    float y = ba.y + bh + 56 * s;
    for (const auto& l : lines) {
      if (l.kind != 'L' && l.kind != 'N' && l.kind != 'D') continue;
      const float ls = 0.62f * s;
      DrawText(dl, tex, ImVec2(W * 0.5f - TextWidth(l.text, ls) * 0.5f, y), ls,
               l.kind == 'D' ? kDim : kParchment, l.text);
      y += lh * ls + 6 * s;
    }
    return;
  }

  // Menu board over the stock rows' column (x 214..640), then the lines.
  const float x0 = 232 * sx, x1 = 618 * sx;
  float y = 104 * sy;
  float content_h = 0;
  bool has_footer = false;
  for (const auto& l : lines) {
    content_h += l.kind == 'R' ? 58 * sy : (l.kind == 'F' ? 0 : 34 * sy);
    has_footer |= l.kind == 'F';
  }
  // From x 184: the board also hides the stock menu's own A medallion (x 192..246).
  const ImVec2 board_a(184 * sx, 34 * sy),
      board_b(650 * sx, std::max(y + content_h + (has_footer ? 44 : 16) * sy, 420 * sy));
  dl->AddRectFilled(ImVec2(board_a.x + 3 * s, board_a.y + 4 * s), ImVec2(board_b.x + 3 * s, board_b.y + 4 * s),
                    IM_COL32(0, 0, 0, 120), 18 * s, ImDrawFlags_RoundCornersRight);
  dl->AddRectFilled(board_a, board_b, kLeather, 18 * s, ImDrawFlags_RoundCornersRight);
  dl->AddRect(board_a, board_b, kCopper, 18 * s, ImDrawFlags_RoundCornersRight, 2.5f * s);
  if (!title.empty()) DrawText(dl, tex, ImVec2(x0 + 18 * sx, 46 * sy), row_scale * 1.1f, kGold, title);

  std::string footer;
  for (const auto& l : lines) {
    switch (l.kind) {
      case 'R': {
        const ImVec2 a(x0, y), b(x1, y + 54 * sy);
        DrawBar(dl, a, b, s, !l.enabled);
        DrawText(dl, tex, ImVec2(x0 + 28 * sx, y + (54 * sy - lh * row_scale) * 0.5f), row_scale,
                 l.enabled ? kGold : kDim, l.text);
        if (l.selected) DrawMedallion(dl, tex, ImVec2(x0 - 13 * sx, y + 27 * sy), s);
        y += 58 * sy;
        break;
      }
      case 'S':
        DrawText(dl, tex, ImVec2(x0 + 18 * sx, y + 4 * sy), 0.7f * s, kGold, l.text);
        dl->AddLine(ImVec2(x0 + 18 * sx, y + 30 * sy), ImVec2(x1 - 30 * sx, y + 30 * sy), kCopperDark, 2 * s);
        y += 34 * sy;
        break;
      case 'N':
      case 'L':
      case 'D':
        DrawText(dl, tex, ImVec2(x0 + 18 * sx, y + 4 * sy), 0.66f * s, l.kind == 'D' ? kDim : kParchment,
                 l.text);
        y += 34 * sy;
        break;
      case 'F':
        footer = l.text;
        break;
      default:
        break;
    }
  }
  if (!footer.empty()) {
    DrawText(dl, tex, ImVec2(x0 + 18 * sx, board_b.y - 30 * sy), 0.55f * s, kDim, footer);
  }
}

}  // namespace rex::ui

extern "C" void rex_ui_set_lobby_view(const char* view) {
  std::lock_guard<std::mutex> lock(rex::ui::g_view_mutex);
  rex::ui::g_view = view ? view : "";
}

extern "C" void rex_ui_set_lobby_cover(const char* text) {
  std::lock_guard<std::mutex> lock(rex::ui::g_view_mutex);
  rex::ui::g_cover = text ? text : "";
}

extern "C" void rex_ui_set_lobby_font(const char* bft_path) {
  std::lock_guard<std::mutex> lock(rex::ui::g_view_mutex);
  rex::ui::g_font_path = bft_path ? bft_path : "";
}

extern "C" void rex_ui_set_toast(const char* text, int ms) {
  const bool show = text && *text && ms > 0;
  {
    std::lock_guard<std::mutex> lock(rex::ui::g_view_mutex);
    rex::ui::g_toast = show ? text : "";
  }
  rex::ui::g_toast_until_ms.store(show ? rex::ui::SteadyMs() + ms : 0);
}
