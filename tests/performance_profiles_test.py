#!/usr/bin/env python3
"""Check that browser profile controls and the typed runtime fallback agree."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    catalog = json.loads((ROOT / "web/depth-profiles.json").read_text())
    effective = json.loads(subprocess.check_output(
        [sys.argv[1], "--dump-effective-config", str(ROOT / "config/nuvo_pi5_depth_100mm.yaml")],
        text=True, timeout=10,
    ))["configuration"]
    assert catalog["schema_version"] == 1
    assert catalog["calibration_preset_id"] == effective["product"]["profile_id"]
    assert catalog["capture"] == {
        key: effective["capture"][key] for key in ("width", "height", "target_hz")
    }
    assert set(catalog["profiles"]) == {"quality", "balanced", "fast", "wide"}
    default = catalog["profiles"][catalog["default_profile"]]
    controls = catalog["base_controls"] | default["controls"]
    assert controls == json.loads((ROOT / "config/stereo.json").read_text())
    stereo = effective["stereo"]
    mode_to_control = {
        "sgbm_3way": "3way", "sgbm_standard": "standard",
        "sgbm_hh4": "hh4", "sgbm_hh": "hh",
    }
    assert mode_to_control[stereo["mode"]] == controls["sgbm_mode"]
    for key in ("min_disparity", "max_disparity", "block_size", "uniqueness_ratio",
                "speckle_window_size", "speckle_range", "disparity_12_max_diff",
                "pre_filter_cap", "median_filter_size", "opencv_threads"):
        assert stereo[key] == controls[key], key
    assert stereo["working_disparities"] == controls["search_range"]
    for name, profile in catalog["profiles"].items():
        assert profile["qualification"] == "unqualified_candidate", name
        assert profile["depth_rate_guarantee_hz"] is None, name
        assert profile["matching"] == {
            "width": stereo["working_width"], "height": stereo["working_height"]
        }, name
        values = catalog["base_controls"] | profile["controls"]
        assert values["search_range"] % 16 == 0
        assert values["min_disparity"] < values["max_disparity"] <= values["search_range"]
        assert values["sgbm_mode"] in mode_to_control.values()
        assert values["median_filter_size"] in (0, 3, 5)
        assert "unqualified" in profile["description"].lower() or name == "quality"
    measurement = catalog["profiles"]["quality"]["historical_measurement"]
    assert not measurement["qualifies_current_release"]
    assert measurement["capture_target_hz"] != catalog["capture"]["target_hz"]
    script = (ROOT / "web/app.js").read_text(encoding="utf-8")
    assert "fetch('/depth-profiles.json'" in script
    assert "depthPresets={" not in script, "browser has a second preset authority"
    assert "...depthProfiles.base_controls,...profile.controls" in script
    assert "depthProfiles=null;$('presetError')" in script
    for filename in ("fast-728x544-15.yaml", "quality-1456x1088-5.yaml"):
        assert "kind: historical_geometry_proposal" in (ROOT / "presets" / filename).read_text()
    print("Performance profile catalog agrees with runtime fallback; throughput remains unqualified")


if __name__ == "__main__":
    main()
