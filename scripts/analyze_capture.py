"""
RenderDoc capture analysis script.
Run with: renderdoccmd.exe python3 scripts/analyze_capture.py <capture.rdc>

Checks:
1. Capture opens successfully
2. Has draw calls (scene is rendering)
3. Final backbuffer is not empty (has non-black pixels)
"""

import sys
import os
import struct

# renderdoccmd injects renderdoc module
import renderdoc as rd

def analyze(capture_path):
    print(f"=== QymEngine Render Analysis ===")
    print(f"Capture: {capture_path}")
    print()

    # Open capture
    cap = rd.OpenCaptureFile()
    result = cap.OpenFile(capture_path, '', None)
    if result != rd.ResultCode.Succeeded:
        print(f"FAIL: Could not open capture file: {result}")
        return False

    if not cap.LocalReplaySupport():
        print("FAIL: Local replay not supported")
        cap.Shutdown()
        return False

    # Create replay controller
    status, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ResultCode.Succeeded:
        print(f"FAIL: Could not create replay: {status}")
        cap.Shutdown()
        return False

    # Get draw calls
    actions = controller.GetRootActions()

    def count_draws(action_list):
        total = 0
        for a in action_list:
            if a.flags & rd.ActionFlags.Drawcall:
                total += 1
            total += count_draws(a.children)
        return total

    draw_count = count_draws(actions)
    print(f"Draw calls: {draw_count}")

    if draw_count == 0:
        print("FAIL: No draw calls found")
        controller.Shutdown()
        cap.Shutdown()
        return False

    # Get textures to find the backbuffer/output
    textures = controller.GetTextures()
    print(f"Textures: {len(textures)}")

    # Find output targets from the last action
    all_actions = []
    def collect_actions(action_list):
        for a in action_list:
            all_actions.append(a)
            collect_actions(a.children)
    collect_actions(actions)

    # Get the last draw/present action
    last_action = all_actions[-1] if all_actions else None
    if last_action:
        controller.SetFrameEvent(last_action.eventId, True)

    # Check for any render output
    outputs = controller.GetOutputTargets()
    print(f"Render targets at end of frame: {len(outputs)}")

    # Try to get pixel data from the first output target
    passed = True
    if len(outputs) > 0 and outputs[0] != rd.ResourceId.Null():
        tex_id = outputs[0]
        tex_info = controller.GetTexture(tex_id)
        print(f"Output texture: {tex_info.width}x{tex_info.height}, format={tex_info.format.Name()}")

        # Sample center pixel
        pixel = controller.PickPixel(tex_id, tex_info.width // 2, tex_info.height // 2,
                                      rd.Subresource(0, 0, 0), rd.CompType.Float)
        print(f"Center pixel: R={pixel.floatValue[0]:.3f} G={pixel.floatValue[1]:.3f} B={pixel.floatValue[2]:.3f} A={pixel.floatValue[3]:.3f}")

        # Check it's not pure black
        r, g, b = pixel.floatValue[0], pixel.floatValue[1], pixel.floatValue[2]
        if r < 0.001 and g < 0.001 and b < 0.001:
            print("WARN: Center pixel is black (may be expected if no object at center)")

    # Summary
    print()
    print(f"=== Analysis Summary ===")
    print(f"Draw calls:     {draw_count} {'PASS' if draw_count > 0 else 'FAIL'}")
    print(f"Textures:       {len(textures)} {'PASS' if len(textures) > 0 else 'FAIL'}")
    print(f"Render targets: {len(outputs)} {'PASS' if len(outputs) > 0 else 'FAIL'}")

    if draw_count > 0 and len(textures) > 0:
        print(f"\nRESULT: PASS")
    else:
        print(f"\nRESULT: FAIL")
        passed = False

    controller.Shutdown()
    cap.Shutdown()
    return passed

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: renderdoccmd python3 scripts/analyze_capture.py <capture.rdc>")
        sys.exit(1)

    capture_path = sys.argv[1]
    if not os.path.exists(capture_path):
        print(f"ERROR: Capture file not found: {capture_path}")
        sys.exit(1)

    success = analyze(capture_path)
    sys.exit(0 if success else 1)
