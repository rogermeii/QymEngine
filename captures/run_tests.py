"""
QymEngine Automated Test Suite
Tests all major features via UI automation (input simulation + queries).
"""

import json
import time
import os
import sys

COMMAND_FILE = "E:/MYQ/QymEngine/captures/command.json"
RESULT_FILE = "E:/MYQ/QymEngine/captures/command_result.json"
SCREENSHOT_DIR = "E:/MYQ/QymEngine/captures/tests"

os.makedirs(SCREENSHOT_DIR, exist_ok=True)

def send_command(cmd_json, timeout=3):
    if os.path.exists(RESULT_FILE):
        os.remove(RESULT_FILE)
    with open(COMMAND_FILE, "w") as f:
        json.dump(cmd_json, f)
    for _ in range(int(timeout * 10)):
        time.sleep(0.1)
        if os.path.exists(RESULT_FILE) and not os.path.exists(COMMAND_FILE):
            with open(RESULT_FILE, "r") as f:
                return json.load(f)
    return {"status": "timeout"}

def screenshot(name):
    path = f"{SCREENSHOT_DIR}/{name}.png"
    result = send_command({"command": "screenshot", "params": {"path": path}})
    return path if result.get("status") == "ok" else None

def get_layout():
    return send_command({"command": "get_ui_layout"})

def click(x, y, button="left"):
    send_command({"command": "mouse_click", "params": {"x": int(x), "y": int(y), "button": button}})
    time.sleep(0.2)

def mouse_down(x, y, button="left"):
    send_command({"command": "mouse_down", "params": {"x": int(x), "y": int(y), "button": button}})
    time.sleep(0.15)

def mouse_move(x, y, button="", dx=0, dy=0):
    send_command({"command": "mouse_move", "params": {"x": int(x), "y": int(y), "button": button, "dx": dx, "dy": dy}})
    time.sleep(0.15)

def mouse_up(x, y, button="left"):
    send_command({"command": "mouse_up", "params": {"x": int(x), "y": int(y), "button": button}})
    time.sleep(0.15)

def scroll(x, y, delta):
    send_command({"command": "mouse_scroll", "params": {"x": int(x), "y": int(y), "delta": delta}})
    time.sleep(0.2)

def key_combo(key, ctrl=False, shift=False):
    send_command({"command": "key_combo", "params": {"key": key, "ctrl": ctrl, "shift": shift}})
    time.sleep(0.5)

def click_widget(widget_id, layout_data):
    for w in layout_data.get("widgets", []):
        if w["id"] == widget_id:
            cx = int(w["x"] + w["w"] / 2)
            cy = int(w["y"] + w["h"] / 2)
            click(cx, cy)
            return True
    return False

def get_panel_center(panel_name, layout_data):
    p = layout_data.get("panels", {}).get(panel_name)
    if p:
        return int(p["x"] + p["w"] / 2), int(p["y"] + p["h"] / 2)
    return None, None

passed = 0
failed = 0
total = 0

def test(name, condition, detail=""):
    global passed, failed, total
    total += 1
    if condition:
        passed += 1
        print(f"  PASS: {name}")
    else:
        failed += 1
        print(f"  FAIL: {name} -- {detail}")

# =========================================
print("=" * 60)
print("QymEngine Automated Test Suite")
print("=" * 60)

# Wait for editor to fully initialize
time.sleep(1)

# --- Test 1: Scene loaded ---
print("\n[Test 1] Scene loading")
info = send_command({"command": "get_scene_info"})
test("Scene info returns ok", info.get("status") == "ok")
nodes = info.get("nodes", [])
node_names = [n["name"] for n in nodes]
test("Scene has expected nodes",
     all(n in node_names for n in ["Ground", "Center Cube", "Sphere", "Sun Light"]),
     f"got: {node_names}")

# --- Test 2: Draw stats ---
print("\n[Test 2] Draw statistics")
stats = send_command({"command": "get_draw_stats"})
dc = stats.get("draw_calls", 0)
tri = stats.get("triangles", 0)
test("Draw calls > 0", dc > 0, f"got {dc}")
test("Triangles > 0", tri > 0, f"got {tri}")
print(f"  INFO: {dc} draw calls, {tri} triangles")

# --- Test 3: Screenshot ---
print("\n[Test 3] Screenshot")
path = screenshot("test3_initial")
test("Screenshot saved", path and os.path.exists(path))
if path:
    size = os.path.getsize(path)
    test("Screenshot has content", size > 1000, f"size={size}")

# --- Test 4: Node selection via click ---
print("\n[Test 4] Node selection via UI click")
layout = get_layout()

click_widget("hierarchy/Sphere", layout)
time.sleep(0.3)
info2 = send_command({"command": "get_scene_info"})
sphere = next((n for n in info2.get("nodes", []) if n["name"] == "Sphere"), None)
test("Sphere is selected", sphere and sphere.get("selected", False))

path2 = screenshot("test4_sphere_selected")
test("Selection screenshot saved", path2 and os.path.exists(path2))

click_widget("hierarchy/Ground", layout)
time.sleep(0.3)
info3 = send_command({"command": "get_scene_info"})
ground = next((n for n in info3.get("nodes", []) if n["name"] == "Ground"), None)
sphere2 = next((n for n in info3.get("nodes", []) if n["name"] == "Sphere"), None)
test("Ground is selected", ground and ground.get("selected", False))
test("Sphere is deselected", sphere2 and not sphere2.get("selected", False))

# --- Test 5: Camera orbit via multi-step right-drag ---
print("\n[Test 5] Camera orbit via right-drag (multi-step)")
sv_cx, sv_cy = get_panel_center("SceneView", layout)
test("SceneView panel found", sv_cx is not None)

path3 = screenshot("test5_before_orbit")

if sv_cx:
    # Multi-step drag: move to SceneView → press right → move → release
    mouse_move(sv_cx, sv_cy)  # position cursor in SceneView first
    time.sleep(0.2)
    mouse_down(sv_cx, sv_cy, button="right")  # press right button
    time.sleep(0.2)
    # Drag with multiple small moves
    for i in range(5):
        mouse_move(sv_cx + (i + 1) * 40, sv_cy, button="right", dx=40, dy=0)
        time.sleep(0.1)
    mouse_up(sv_cx + 200, sv_cy, button="right")  # release
    time.sleep(0.3)

path4 = screenshot("test5_after_orbit")
if path3 and path4:
    s1, s2 = os.path.getsize(path3), os.path.getsize(path4)
    test("Camera view changed after orbit", s1 != s2, f"before={s1}, after={s2}")

# --- Test 6: Zoom via scroll ---
print("\n[Test 6] Camera zoom via scroll")
path5 = screenshot("test6_before_zoom")
if sv_cx:
    # Move to SceneView first
    mouse_move(sv_cx, sv_cy)
    time.sleep(0.2)
    scroll(sv_cx, sv_cy, -3)
    time.sleep(0.5)
path6 = screenshot("test6_after_zoom")
if path5 and path6:
    s1, s2 = os.path.getsize(path5), os.path.getsize(path6)
    test("Camera view changed after zoom", s1 != s2, f"before={s1}, after={s2}")

# --- Test 7: Shader hot reload via Ctrl+R ---
print("\n[Test 7] Shader hot reload via Ctrl+R")
stats_before = send_command({"command": "get_draw_stats"})
dc_before = stats_before.get("draw_calls", 0)

key_combo("r", ctrl=True)
time.sleep(3)

stats_after = send_command({"command": "get_draw_stats"})
dc_after = stats_after.get("draw_calls", 0)
test("Draw calls maintained after shader reload", dc_after > 0, f"got {dc_after}")
path7 = screenshot("test7_after_reload")
test("Rendering intact after reload", path7 and os.path.exists(path7))

# --- Test 8: Material system ---
print("\n[Test 8] Material system")
info4 = send_command({"command": "get_scene_info"})
# Field name in JSON is "material" (not "materialPath")
nodes_with_mat = [n for n in info4.get("nodes", []) if n.get("material")]
test("Nodes have material paths", len(nodes_with_mat) > 0, f"got {len(nodes_with_mat)}")
for n in nodes_with_mat[:3]:
    mat = n.get("material", "")
    test(f"  {n['name']} has .mat.json", mat.endswith(".mat.json"), mat)

# --- Test 9: RenderDoc capture via F12 ---
print("\n[Test 9] RenderDoc capture via F12")
rdc_before = set(f for f in os.listdir("E:/MYQ/QymEngine/captures") if f.endswith(".rdc"))
key_combo("F12")
time.sleep(3)
rdc_after = set(f for f in os.listdir("E:/MYQ/QymEngine/captures") if f.endswith(".rdc"))
new_rdcs = rdc_after - rdc_before
test("New RDC file created by F12", len(new_rdcs) > 0, f"new: {new_rdcs}")

# --- Test 10: Rapid selection changes ---
print("\n[Test 10] Rapid selection changes")
layout3 = get_layout()
for name in ["Center Cube", "Tall Pillar", "Pyramid", "Tilted Cube"]:
    click_widget(f"hierarchy/{name}", layout3)
    time.sleep(0.15)

info5 = send_command({"command": "get_scene_info"})
selected = [n for n in info5.get("nodes", []) if n.get("selected")]
test("Exactly one node selected", len(selected) == 1, f"got {len(selected)}")
if selected:
    test("Last clicked node selected", selected[0]["name"] == "Tilted Cube",
         f"got {selected[0]['name']}")

# Final screenshot
screenshot("test_final")

# =========================================
print("\n" + "=" * 60)
print(f"Results: {passed}/{total} passed, {failed} failed")
print("=" * 60)

sys.exit(0 if failed == 0 else 1)
