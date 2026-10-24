#!/usr/bin/env python3
"""Small dependency-free contract check for the standalone device UI."""

from __future__ import annotations

from collections import Counter
from html.parser import HTMLParser
from pathlib import Path
import re
import json
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1]


class InterfaceParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.ids: list[str] = []
        self.pages: set[str] = set()
        self.navigation: set[str] = set()
        self.navigation_order: list[str] = []
        self.controls: dict[str, dict] = {}

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        attributes = dict(attrs)
        if identifier := attributes.get("id"):
            self.ids.append(identifier)
            if tag in ("input","select"):
                self.controls[identifier]={**attributes,**({"type":"select"} if tag=="select" else {})}
            if tag == "section" and "page" in (attributes.get("class") or "").split():
                self.pages.add(identifier)
        if page := attributes.get("data-page"):
            self.navigation.add(page)
            self.navigation_order.append(page)


def main() -> None:
    html = (ROOT / "web" / "index.html").read_text(encoding="utf-8")
    script = (ROOT / "web" / "app.js").read_text(encoding="utf-8")
    parser = InterfaceParser()
    parser.feed(html)
    duplicates = [identifier for identifier, count in Counter(parser.ids).items() if count > 1]
    assert not duplicates, f"duplicate element IDs: {duplicates}"
    assert parser.pages == parser.navigation, f"page/navigation mismatch: {parser.pages ^ parser.navigation}"
    assert parser.navigation_order == [
        "overview", "viewer", "network", "device",
    ], f"unexpected operator navigation order: {parser.navigation_order}"
    overview = html[html.index('<section id="overview"'):html.index('<section id="viewer"')]
    assert '<section id="overview" class="page active">' in overview
    for identifier in ("setupState", "cameraState", "frameRate", "processingTime", "calibrationBanner",
                       "overviewCaptureState", "overviewDepthState", "performanceMonitor"):
        assert f'id="{identifier}"' in overview, f"Overview must own {identifier}"
    assert '<canvas' not in overview, "Overview must not restore duplicated camera previews"
    assert 'id="overview" class="device-section"' not in html
    literal_refs = set(re.findall(r"\$\('([^']+)'\)", script))
    missing = literal_refs - set(parser.ids)
    assert not missing, f"JavaScript refers to missing IDs: {sorted(missing)}"
    assert 'id="pipeline"' not in html and '/pipeline' not in html
    assert 'NuvoPipeline' not in script and '/pipeline.html' not in script, "production console must not depend on the parked pipeline"
    assert '/pipeline.js' not in html
    for identifier in ("performanceMonitor", "performanceStages", "performanceChart",
                       "viewerReceiving", "outputRequested", "outputPending", "outputDiscard"):
        assert identifier in parser.ids
    assert 'timing-track' not in html, "single-value latency bars should not return"
    for identifier in ("themeToggle", "flowConnectionState", "flowCaptureState",
                       "flowQualityState", "flowDeliveryState"):
        assert identifier in parser.ids
    assert "nuvo-console-theme" in script and "prefers-color-scheme" in script
    assert 'id="redGain" type="number" min="0.1" max="16" step="0.0001"' in html
    assert 'id="blueGain" type="number" min="0.1" max="16" step="0.0001"' in html
    for identifier in ("exposureReset", "exposureTiming", "manualExposureMs",
                       "autoMinExposureMs", "autoMaxExposureMs"):
        assert identifier in parser.ids
    assert "Advanced capture controls" in html
    assert "Auto exposure limits" in html and "Auto response" in html
    assert "White balance" in html
    assert "exposure_safe_max_us" in script
    assert "Saved · waiting for camera confirmation" in script
    assert "Reset to saved device settings" in script
    encoded_choices = re.search(r'<select id="streamSource">(.*?)</select>', html, re.S)
    assert encoded_choices, "missing encoded source selector"
    assert set(re.findall(r'<option value="([^"]+)"(?: hidden)?>', encoded_choices.group(1))) == {
        "left_rectified", "right_rectified", "disparity", "depth", "left_color", "right_color",
        "left_mono", "right_mono", "left_rgb", "right_rgb", "left_rectified_rgb",
        "right_rectified_rgb", "rectified_stereo", "rectified_stereo_rgb",
    }, "encoded source selector advertises unsupported encoder inputs"
    assert '<optgroup label="Measurements shown as pictures">' in encoded_choices.group(1)
    assert '<option value="left_rgb" hidden>' in encoded_choices.group(1)
    for source in ("left_color", "right_color", "left_rectified",
                   "right_rectified", "disparity", "depth"):
        assert f'<option value="{source}">' in html
    assert '<option value="left_color">Left camera · color</option>' in html
    assert "source==='left_color'?'Left color · ISP':'Right color · ISP'" in script
    assert 'id="viewerEmptyState"' in html and 'Waiting for selected live products.' in html
    assert 'id="encodedEmptyState"' in html and 'Waiting for encoded preview' in html
    assert 'tuningPreviewTemplate' not in html and 'outputEncodedCanvas' not in html
    assert 'data-preview-slot' not in html, "one viewer must not clone obsolete preview panels"
    assert 'id="outputDestination"' in html and 'id="pixelInspector"' in html
    assert 'id="wifiApply"' in html and 'id="wifiConfirm"' in html
    for identifier in ('outputApplicationDelivery','outputPathChoices','outputPathAddress','outputPathStatus',
                       'outputUsePicture','wiredPreference','wiredApply','deliveryTransactionState'):
        assert identifier in parser.ids
    assert set(re.findall(r'name="outputPath" value="([^"]+)"', html)) == {'ethernet','usb_device','wifi'}
    assert re.findall(r'name="outputPath" value="[^"]+"><span>([^<]+)<small>([^<]+)</small>', html) == [
        ('Ethernet', 'Pi Ethernet port'), ('USB 2.0', 'Pi USB-C port'), ('WLAN', 'Pi Wi-Fi connection')]
    assert '<details id="outputConnectionAdvanced" class="network-advanced">' in html
    assert 'USB-C device network' not in html
    assert 'data-transport=' not in html, 'keep one scoped device delivery control instead of duplicate legacy buttons'
    assert 'DIODE' not in html and '/assets/benchmark/' not in html
    assert 'benchmarkGallery' not in script and 'data-benchmark-scene' not in script
    assert '<dialog' not in html and 'type="password"' not in html
    assert 'claimDialog' not in script and 'ownerDialog' not in script
    assert 'needs_owner_password' not in script and '/api/v1/setup/owner' not in script
    assert 'Disconnected · showing last received products' in script
    for identifier in ('outputPreset','outputAdvanced','outputFrameSummary'):
        assert identifier in parser.ids
    assert 'Depth preview · 8-bit visualization' in html
    assert 'Local socket delivery is independent' in html
    assert 'not sensor Bayer RAW' in html
    assert '−1 disables' not in html
    assert 'OpenCV SGBM already applies its own 3 × 3 median' in html
    # The overview's calibration alert leads directly to the corrective action.
    assert 'data-open="calibration"' in html
    assert 'class="cards status-strip"' in html
    assert re.search(r'<details class="setup-guide">\s*<summary>Setup guide</summary>', html)
    assert 'From camera to useful depth.' not in html
    for identifier in ("encodingReset", "algorithmReset", "deviceResolution",
                       "overviewCaptureState", "overviewDepthState"):
        assert identifier in parser.ids
    for weight in (400, 500, 600):
        assert (ROOT / "web/assets/fonts" / f"hanken-grotesk-{weight}.ttf").is_file()
    if node := shutil.which("node"):
        definitions=json.loads(subprocess.check_output([node,"-e","process.stdout.write(JSON.stringify(require('./web/device-settings.js').DEFINITIONS))"],cwd=ROOT,text=True))
        for definition in definitions.values():
            for identifier,field in definition["fields"].items():
                native=parser.controls[identifier]
                for attribute in ("type","min","max","step"):
                    assert field.get(attribute)==native.get(attribute), f"shared field mismatch: {identifier} {attribute}"
        subprocess.run([node,str(ROOT/"tests/web_configuration_revision_test.js")],check=True)
        subprocess.run([node,str(ROOT/"tests/web_session_test.js")],check=True)
        subprocess.run([node,str(ROOT/"tests/web_performance_model_test.js")],check=True)
        subprocess.run([node,str(ROOT/"tests/web_output_selection_test.js")],check=True)
        subprocess.run([node,str(ROOT/"tests/web_frame_codec_test.js")],check=True)
        subprocess.run([node,str(ROOT/"tests/web_stream_lifecycle_test.js")],check=True)
        subprocess.run([node,str(ROOT/"tests/web_shared_settings_test.js")],check=True)
        subprocess.run([node,str(ROOT/"tests/web_calibration_status_test.js")],check=True)
        subprocess.run([node,str(ROOT/"tests/web_first_use_test.js")],check=True)
        for asset in ("app.js","device-settings.js","frame-codec.js","first-use.js"):
            subprocess.run([node,"--check",str(ROOT/"web"/asset)],check=True)
    print(f"UI contract OK: {len(parser.pages)} pages, {len(parser.ids)} unique IDs")


if __name__ == "__main__":
    main()
