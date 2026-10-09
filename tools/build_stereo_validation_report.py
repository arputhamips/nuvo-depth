#!/usr/bin/env python3
"""Build the Nuvo full/half-resolution stereo validation report."""

from __future__ import annotations

import csv
import json
from pathlib import Path

from PIL import Image as PilImage
from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER
from reportlab.lib.pagesizes import landscape, letter
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import inch
from reportlab.platypus import (Image, PageBreak, Paragraph, SimpleDocTemplate,
                                Spacer, Table, TableStyle)


ROOT = Path(__file__).resolve().parents[1]
VALIDATION = ROOT / "deliverables/stereo-validation-20260914"
CURRENT_HIL = ROOT / "deliverables/pi5-flicker-20260915-hh60"
OUTPUT = ROOT / "output/pdf/nuvo-depth-stereo-validation-20260915.pdf"
PAGE_W, PAGE_H = landscape(letter)

INK = colors.HexColor("#1d2733")
MUTED = colors.HexColor("#5d6a78")
BLUE = colors.HexColor("#2376c9")
PALE = colors.HexColor("#eaf3fb")
LINE = colors.HexColor("#cbd5df")
WARN = colors.HexColor("#fff3d6")
GREEN = colors.HexColor("#e9f6ee")


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def read_csv(path: Path) -> list[dict]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def number(value, digits=3) -> str:
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return "—"
    return f"{parsed:.{digits}f}"


def percent(value, digits=1) -> str:
    try:
        return f"{100 * float(value):.{digits}f}%"
    except (TypeError, ValueError):
        return "—"


def average_summary(clean: dict, final: dict) -> dict:
    return {
        "coverage": (clean["coverage"] + final["coverage"]) / 2,
        "bad2": (clean["bad_2_including_invalid"] +
                 final["bad_2_including_invalid"]) / 2,
        "mae": (clean["mae_valid_only_px"] + final["mae_valid_only_px"]) / 2,
        "p50": (clean["matcher_ms_p50"] + final["matcher_ms_p50"]) / 2,
        "p95": (clean["matcher_ms_p95"] + final["matcher_ms_p95"]) / 2,
        "frames": clean["frame_count"] + final["frame_count"],
    }


def paired(directory: Path) -> dict:
    return average_summary(load_json(directory / "clean/summary.json"),
                           load_json(directory / "final/summary.json"))


styles = getSampleStyleSheet()
styles.add(ParagraphStyle(name="Cover", parent=styles["Title"], fontName="Helvetica-Bold",
                          fontSize=27, leading=31, textColor=INK, alignment=TA_CENTER,
                          spaceAfter=16))
styles.add(ParagraphStyle(name="Subtitle", parent=styles["Normal"], fontSize=13,
                          leading=18, textColor=MUTED, alignment=TA_CENTER))
styles.add(ParagraphStyle(name="H1x", parent=styles["Heading1"], fontName="Helvetica-Bold",
                          fontSize=20, leading=23, textColor=INK, spaceAfter=10))
styles.add(ParagraphStyle(name="H2x", parent=styles["Heading2"], fontName="Helvetica-Bold",
                          fontSize=13, leading=16, textColor=BLUE, spaceBefore=8,
                          spaceAfter=6))
styles.add(ParagraphStyle(name="Bodyx", parent=styles["BodyText"], fontSize=9.3,
                          leading=13, textColor=INK, spaceAfter=7))
styles.add(ParagraphStyle(name="Smallx", parent=styles["BodyText"], fontSize=7.3,
                          leading=9.5, textColor=MUTED))
styles.add(ParagraphStyle(name="Callout", parent=styles["BodyText"], fontSize=10.2,
                          leading=14, textColor=INK, backColor=PALE, borderColor=BLUE,
                          borderWidth=0.8, borderPadding=9, spaceBefore=6, spaceAfter=10))
styles.add(ParagraphStyle(name="Warn", parent=styles["BodyText"], fontSize=9.3,
                          leading=13, textColor=INK, backColor=WARN, borderColor=colors.HexColor("#d39a22"),
                          borderWidth=0.8, borderPadding=8, spaceAfter=9))


def p(text: str, style="Bodyx") -> Paragraph:
    return Paragraph(text, styles[style])


def table(rows, widths=None, font_size=7.4, header=True, aligns=None) -> Table:
    value = Table(rows, colWidths=widths, repeatRows=1 if header else 0,
                  hAlign="LEFT")
    commands = [
        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
        ("BACKGROUND", (0, 0), (-1, 0), INK),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("FONTNAME", (0, 1), (-1, -1), "Helvetica"),
        ("FONTSIZE", (0, 0), (-1, -1), font_size),
        ("LEADING", (0, 0), (-1, -1), font_size + 2),
        ("GRID", (0, 0), (-1, -1), 0.35, LINE),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, colors.HexColor("#f6f8fa")]),
        ("VALIGN", (0, 0), (-1, -1), "MIDDLE"),
        ("LEFTPADDING", (0, 0), (-1, -1), 4),
        ("RIGHTPADDING", (0, 0), (-1, -1), 4),
        ("TOPPADDING", (0, 0), (-1, -1), 3),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 3),
    ]
    if aligns:
        for column, alignment in enumerate(aligns):
            commands.append(("ALIGN", (column, 1), (column, -1), alignment))
    value.setStyle(TableStyle(commands))
    return value


def fitted(path: Path, max_width: float, max_height: float) -> Image:
    with PilImage.open(path) as source:
        width, height = source.size
    scale = min(max_width / width, max_height / height)
    return Image(str(path), width=width * scale, height=height * scale)


def visual_page(story, title: str, path: Path, note: str) -> None:
    story += [p(title, "H1x"), p(note, "Smallx"), Spacer(1, 5),
              fitted(path, 9.6 * inch, 6.25 * inch), PageBreak()]


def on_page(canvas, document) -> None:
    canvas.saveState()
    canvas.setStrokeColor(LINE)
    canvas.line(0.55 * inch, 0.42 * inch, PAGE_W - 0.55 * inch, 0.42 * inch)
    canvas.setFont("Helvetica", 7)
    canvas.setFillColor(MUTED)
    canvas.drawString(0.58 * inch, 0.22 * inch,
                      "Nuvo Depth · Pi 5 stereo validation · engineering evidence")
    canvas.drawRightString(PAGE_W - 0.58 * inch, 0.22 * inch,
                           f"Page {document.page}")
    canvas.restoreState()


def matrix_appendix(story, title: str, rows: list[dict], matching: str) -> None:
    story += [p(title, "H1x"), p(
        f"{len(rows)} unique profiles. Clean and Final results are averaged over "
        f"the evenly distributed screen set. Matching grid: {matching}.", "Smallx")]
    header = ["Profile", "Alg", "Range", "Block", "Unique", "Speckle",
              "LR", "Pre", "BM tex", "Bad-2", "Coverage", "MAE", "p50 ms"]
    values = [header]
    for row in sorted(rows, key=lambda item: float(item["screen_bad2_mean"])):
        values.append([
            row["name"], row["algorithm"].upper(), row["search_range"],
            row["block_size"], row["uniqueness_ratio"],
            f"{row['speckle_window_size']}/{row['speckle_range']}",
            row["disparity_12_max_diff"], row["pre_filter_cap"],
            row["bm_texture_threshold"], percent(row["screen_bad2_mean"], 2),
            percent(row["screen_coverage_mean"], 2),
            number(row["screen_mae_mean"], 3),
            number(row["screen_latency_mean_ms"], 2),
        ])
    story += [Spacer(1, 6), table(values,
        widths=[1.72*inch, .34*inch, .4*inch, .38*inch, .42*inch, .52*inch,
                .32*inch, .32*inch, .42*inch, .45*inch, .5*inch, .42*inch, .45*inch],
        font_size=5.35,
        aligns=["LEFT", "CENTER", "RIGHT", "RIGHT", "RIGHT", "RIGHT",
                "RIGHT", "RIGHT", "RIGHT", "RIGHT", "RIGHT", "RIGHT", "RIGHT"]),
        PageBreak()]


def main() -> None:
    required = [
        VALIDATION / "matrix/results.csv",
        VALIDATION / "half-matrix/results.csv",
        VALIDATION / "hh-m5-full/clean/summary.json",
        VALIDATION / "hh-m5-full/final/summary.json",
    ]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise SystemExit("missing report inputs:\n" + "\n".join(missing))

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    report_assets = VALIDATION / "report-assets"
    report_assets.mkdir(parents=True, exist_ok=True)
    source = PilImage.open(VALIDATION / "residual-rectification/before-after-grayscale.png")
    slice_height = source.height // 3
    residual_slices = []
    for index in range(3):
        path = report_assets / f"residual-frame-{index + 1}.png"
        source.crop((0, index * slice_height, source.width,
                     (index + 1) * slice_height)).save(path)
        residual_slices.append(path)

    full_rows = read_csv(VALIDATION / "matrix/results.csv")
    half_rows = read_csv(VALIDATION / "half-matrix/results.csv")
    residual = load_json(VALIDATION / "residual-rectification/summary.json")
    full_camera = load_json(VALIDATION / "best-full-3capture/summary.json")[0]
    half_camera = load_json(VALIDATION / "best-half-3capture/summary.json")[0]
    aligned = load_json(CURRENT_HIL / "aligned-report.json")["summary"]
    pi_perf = load_json(CURRENT_HIL / "live-performance-quality-hh.json")
    full_hh = paired(VALIDATION / "hh-m5-full")
    full_3way = average_summary(
        load_json(VALIDATION / "matrix/native-full-fixed96/sgbm_r96_b5_u0/clean/summary.json"),
        load_json(VALIDATION / "matrix/native-full-fixed96/sgbm_r96_b5_u0/final/summary.json"))
    half_hh = paired(VALIDATION / "half-hh-r48-screen")
    half_recommendation = load_json(VALIDATION / "half-matrix/recommendation.json")

    story = [Spacer(1, 0.75*inch), p("Nuvo Depth stereo validation", "Cover"),
             p("Full-resolution and half-resolution SGBM/StereoBM matrix · Raspberry Pi 5",
               "Subtitle"), Spacer(1, .26*inch),
             p("September 15, 2026 · software candidate · calibration remains provisional",
               "Subtitle"), Spacer(1, .48*inch),
             p("Decision", "H2x"),
             p("Use <b>full-resolution SGBM HH, 96-pixel range, block 5, uniqueness 0, "
               "5 × 5 validity-preserving median</b> when spatial detail is the priority. "
               "It removes the observed horizontal scanline streaks and slightly improves "
               "ground-truth error versus the prior full-resolution 3-way matcher.", "Callout"),
             p("Use <b>half-resolution SGBM HH, 48 working disparities (96 restored), block 5, "
               "uniqueness 0, 3 × 3 median</b> as the smooth live candidate. It is visibly "
               "cleaner, has higher retained-camera coverage, and is much cheaper, at the cost "
               "of lower Sintel accuracy and half-grid spatial detail.", "Callout"),
             p("Release limitation", "H2x"),
             p("The complete candidate is deployed and tested on the Pi 5, but the fresh "
               "60-frame trace proves that brightness variation still enters at or before ISP "
               "Y. Calibration also remains provisional. The package is suitable for "
               "engineering use, not a factory-calibrated or flicker-closed production claim.", "Warn"),
             PageBreak()]

    story += [p("What changed from the clean legacy sample", "H1x"),
              table([
                  ["Surface", "Legacy clean sample", "Regressed state", "Corrected state"],
                  ["Matcher grid", "364 × 272", "728 × 544", "728 × 544 quality + 364 × 272 live candidate"],
                  ["Aggregation", "SGBM 3-way", "SGBM 3-way", "Eight-direction HH"],
                  ["Range", "64 working / 128 restored", "96 full-grid", "96 full-grid or 48 / 96 restored"],
                  ["Post-filter", "Upscale regularization", "None", "Validity-preserving median 5 / 3"],
                  ["Calibration", "Earlier engineering rig", "100 mm provisional seed", "Seed + measured right-output correction"],
              ], widths=[1.2*inch, 2.5*inch, 2.4*inch, 3.2*inch], font_size=7.4),
              Spacer(1, 10),
              p("The supplied images are identical to the repository's WebPage engineering "
                "samples. Their smooth planes came from the former half-resolution matcher. "
                "Switching to full-grid matching removed that low-pass/spatial regularization "
                "and exposed a systematic vertical rectification residual. The remaining long "
                "bands were directional 3-way SGBM failures, not encoder corruption.", "Bodyx"),
              p("Acceptance target and corrected outputs", "H2x"),
              Table([
                  [p("Legacy target", "Smallx"), p("Best full resolution", "Smallx"),
                   p("Best half resolution", "Smallx")],
                  [fitted(ROOT / "WebPage/assets/sample-disparity.png", 3.05*inch, 2.35*inch),
                   fitted(VALIDATION / "best-full-3capture/three-capture-color-grid.png", 3.05*inch, 2.35*inch),
                   fitted(VALIDATION / "best-half-3capture/three-capture-color-grid.png", 3.05*inch, 2.35*inch)]
              ], colWidths=[3.15*inch]*3, style=TableStyle([
                  ("VALIGN", (0,0), (-1,-1), "TOP"), ("ALIGN", (0,0), (-1,-1), "CENTER"),
                  ("BOX", (0,0), (-1,-1), .4, LINE), ("INNERGRID", (0,0), (-1,-1), .3, LINE),
                  ("BACKGROUND", (0,0), (-1,0), colors.HexColor("#f1f4f7")),
              ])), PageBreak()]

    story += [p("Calibration diagnosis and correction", "H1x"),
              p("A robust affine fit across three retained stereo pairs found a spatially "
                "systematic right-view vertical residual. The correction is stored in the "
                "provisional right-camera YAML and composes the dense output-to-source map. "
                "Projection matrices, focal length, baseline, disparity, and depth geometry "
                "are unchanged.", "Bodyx")]
    calibration_rows = [["Sequence", "Vertical p50 before", "Vertical p50 after",
                         "Vertical p95 before", "Vertical p95 after",
                         "Coverage before", "Coverage after", "Photo MAE before", "after"]]
    for item in residual["pairs"]:
        calibration_rows.append([
            item["sequence"], number(item["before"]["vertical_abs_p50_px"]),
            number(item["after"]["vertical_abs_p50_px"]),
            number(item["before"]["vertical_abs_p95_px"]),
            number(item["after"]["vertical_abs_p95_px"]),
            percent(item["before_valid"]), percent(item["corrected_valid"]),
            number(item["before_photo_mae"]), number(item["corrected_photo_mae"]),
        ])
    story += [table(calibration_rows, font_size=6.6), Spacer(1, 8),
              p("Median vertical error falls from approximately 1.77 px to 0.15 px. "
                "This passes the engineering preview gate but does not replace a new target "
                "calibration of the final assembly.", "Warn"), PageBreak()]
    for index, image_path in enumerate(residual_slices, 1):
        visual_page(story, f"Rectification before/after · capture {index}", image_path,
                    "Top: nominal seed. Bottom: correction composed into the right rectification map. "
                    "Disparity and optical-axis depth are shown in neutral grayscale.")

    mode_camera = load_json(VALIDATION / "mode-3capture-corrected-v2/summary.json")
    mode_by_name = {row["profile"]: row for row in mode_camera}
    mode_rows = [["Full matcher", "Camera valid", "Streak fraction", "Vertical grad p95",
                  "3-capture std p95", "Host p50", "64-frame Bad-2"]]
    for label, name in [
        ("3-way", "full-b5-u0-3way-m3"),
        ("Standard 5-path", "full-b5-u0-standard-m3"),
        ("HH4", "full-b5-u0-hh4-m3"),
        ("HH 8-direction", "full-b5-u0-hh-m3"),
    ]:
        camera = mode_by_name[name]
        dataset_dir = (VALIDATION / "hh-screen" if name == "full-b5-u0-hh-m3"
                       else VALIDATION / "mode-screen" / name)
        dataset = paired(dataset_dir)
        mode_rows.append([label, percent(camera["valid_fraction_mean"]),
                          percent(camera["horizontal_streak_fraction_mean"]),
                          number(camera["vertical_disparity_gradient_p95_mean_px"]),
                          number(camera["three_capture_std_p95_px"]),
                          number(camera["matcher_ms_mean"], 1), percent(dataset["bad2"], 2)])
    story += [p("Horizontal streak diagnosis", "H1x"),
              p("The streak gate restricts the orientation score to matcher-valid, low-texture "
                "interior pixels. HH reduces the measured full-grid streak fraction from 11.1% "
                "to 2.5% and cuts three-capture instability by roughly two thirds. HH4 is faster "
                "but introduced visible vertical streaks.", "Bodyx"),
              table(mode_rows, widths=[1.4*inch, .85*inch, .9*inch, .95*inch,
                                       1.05*inch, .75*inch, .9*inch]),
              Spacer(1, 10),
              fitted(VALIDATION / "mode-3capture-corrected-v2/three-capture-grayscale-grid.png",
                     9.55*inch, 4.75*inch), PageBreak()]

    full_rows_main = [
        ["Full-resolution profile", "Evidence frames", "Bad-2 incl invalid", "Coverage",
         "MAE valid", "Host p50", "Camera streak", "Camera std p95"],
        ["3-way · b5 u0 · no median", full_3way["frames"], percent(full_3way["bad2"], 2),
         percent(full_3way["coverage"], 2), number(full_3way["mae"]),
         number(full_3way["p50"], 1), "11.1%*", "25.5 px*"],
        ["HH · b5 u0 · median 5", full_hh["frames"], percent(full_hh["bad2"], 2),
         percent(full_hh["coverage"], 2), number(full_hh["mae"]),
         number(full_hh["p50"], 1), percent(full_camera["horizontal_streak_fraction_mean"]),
         number(full_camera["three_capture_std_p95_px"], 2) + " px"],
    ]
    story += [p("Full-resolution decision", "H1x"),
              p("Complete Sintel runs cover 1,064 Clean and 1,064 Final frames for each "
                "finalist. HH + median 5 improves Bad-2 and MAE while eliminating the camera "
                "scanline failure, but matcher cost is approximately ten times the 3-way host "
                "baseline. Asterisks mark the corresponding median-3 camera mode test.", "Bodyx"),
              table(full_rows_main, widths=[2.1*inch, .8*inch, 1.0*inch, .75*inch,
                                            .72*inch, .75*inch, .8*inch, .85*inch]),
              Spacer(1, 10),
              fitted(VALIDATION / "best-full-3capture/three-capture-grayscale-grid.png",
                     9.55*inch, 3.05*inch), PageBreak()]
    visual_page(story, "Best full-resolution camera result · color",
                VALIDATION / "best-full-3capture/three-capture-color-grid.png",
                "Three retained Pi captures, right-map correction, SGBM HH, range 96, block 5, "
                "uniqueness 0, median 5. Invalid pixels are black.")

    best_half_matrix = half_recommendation["finalists"]
    half_rows_main = [["Half-resolution profile", "Evidence", "Bad-2", "Coverage",
                       "MAE", "Host p50", "Camera valid", "Camera std p95"]]
    for item in best_half_matrix:
        half_rows_main.append([
            item["name"], "full Sintel", percent(item.get("full_bad2_mean"), 2),
            percent(item.get("full_coverage_mean"), 2), number(item.get("full_mae_mean")),
            number(item.get("full_latency_mean_ms"), 2), "—", "—"])
    half_rows_main.append([
        "half HH r48 b5 u0 m3", "128-frame screen", percent(half_hh["bad2"], 2),
        percent(half_hh["coverage"], 2), number(half_hh["mae"]),
        number(half_hh["p50"], 2), percent(half_camera["valid_fraction_mean"]),
        number(half_camera["three_capture_std_p95_px"], 2) + " px"])
    story += [p("Half-resolution matching and invalid-aware upscale", "H1x"),
              p("The historical path downsamples 728 × 544 rectified luma to 364 × 272, matches "
                "there, and restores disparity to 728 × 544 while multiplying disparity by two. "
                "A full-resolution output pixel is valid only when all interpolation support is "
                "valid; holes are never filled across invalid regions.", "Bodyx"),
              table(half_rows_main, font_size=6.7), Spacer(1, 8),
              p("The half-HH r48 profile is the strongest smooth live candidate: 74.2% camera "
                "coverage, 3.17 px three-capture p95 instability, and a host matcher time near "
                "10.7 ms. Its averaged Sintel Bad-2 is worse than full HH, documenting the "
                "spatial-detail tradeoff rather than hiding it.", "Callout"),
              fitted(VALIDATION / "best-half-3capture/three-capture-grayscale-grid.png",
                     9.55*inch, 3.0*inch), PageBreak()]
    visual_page(story, "Best half-resolution camera result · color",
                VALIDATION / "best-half-3capture/three-capture-color-grid.png",
                "Three retained Pi captures, right-map correction, 364 × 272 SGBM HH, 48 working "
                "disparities / 96 restored, invalid-aware upscale, median 3.")

    stages = pi_perf["stages"]
    performance_rows = [["Pipeline stage", "Pi median", "Path / hardware", "Can move to GPU?"]]
    performance_rows += [
        ["ISP + capture", "not instrumented", "IMX296 + PiSP/libcamera + GPIO XTRIG", "Already hardware/ISP"],
        ["Rectification", f"{stages['rectification_ms']['median']:.2f} ms", "EGL/GLES DMABUF · V3D GPU", "Already GPU"],
        ["Stereo preparation", f"{stages['stereo_preprocess_ms']['median']:.2f} ms", "OpenCV CPU", "Little value"],
        ["SGBM HH match", f"{stages['stereo_match_ms']['median']:.2f} ms", "OpenCV CPU · mostly serialized", "No production V3D backend"],
        ["Validity/post-filter", f"{stages['stereo_postprocess_ms']['median']:.2f} ms", "CPU", "Not worthwhile"],
        ["Depth + overhead", f"{stages['depth_and_other_ms']['median']:.2f} ms", "CPU", "Not worthwhile"],
        ["H.264 monitoring", f"{stages['encode_ms']['median']:.2f} ms", "asynchronous libx264 CPU", "Consider V4L2 only after validation"],
        ["Total compute", f"{stages['processing_ms']['median']:.2f} ms", f"{pi_perf['depth_output_rate_hz']:.2f} depth fps", "Dominated by SGBM"],
    ]
    story += [p("Pi 5 pipeline performance and hardware use", "H1x"),
              p("These are 60 distinct live frames from the deployed full-resolution Quality "
                "profile: SGBM HH, range 96, block 5, uniqueness 0, and median 5. Configuration "
                "remained stable throughout the measurement.", "Callout"),
              table(performance_rows, widths=[1.4*inch, 1.0*inch, 3.2*inch, 3.1*inch], font_size=7.2),
              Spacer(1, 10),
              p("The Pi GPU is already doing the operation it suits: dense geometric "
                "rectification. Raspberry Pi V3D has no supported OpenCV SGBM primitive. Porting "
                "stereo to custom compute would be a separate backend and must beat the clean "
                "HH acceptance output. The current full-grid HH path is largely serialized despite "
                "the four-thread setting; the measured Pi depth rate is therefore 2.84 fps. Earlier "
                "half-resolution measurements make half-HH the credible higher-rate quality path.", "Bodyx"),
              p("Resource snapshot", "H2x"),
              p("Current live full-grid HH: depthd 100-118% CPU and 248-369 MiB RSS across ten "
                "one-second samples; gateway 0-2% CPU and 34 MiB RSS. The Pi reported 66.4 C, "
                "2.40 GHz ARM, and get_throttled=0x0. H.264 is asynchronous libx264/NEON at "
                f"{stages['encode_ms']['median']:.2f} ms median, so it is not the depth bottleneck.",
                "Bodyx"), PageBreak()]

    story += [p("60-frame flicker source verification", "H1x"),
              table([
                  ["Stage comparison", "Pairs", "Correlation / result"],
                  ["ISP left to rectified left", aligned["isp_to_rectified"]["pairs"],
                   number(aligned["isp_to_rectified"]["correlation"], 6)],
                  ["ISP right to rectified right", aligned["isp_right_to_rectified_right"]["pairs"],
                   number(aligned["isp_right_to_rectified_right"]["correlation"], 6)],
                  ["Left/right ISP brightness", aligned["left_to_right_isp"]["pairs"],
                   number(aligned["left_to_right_isp"]["correlation"], 6)],
                  ["Pi disparity H.264 packet to PC packet", aligned["stream_packets_compared"],
                   f"{aligned['stream_packets_identical']}/{aligned['stream_packets_compared']} byte-identical"],
                  ["PC decode failures", aligned["pc_stream_pairs"], aligned["encoded_decode_failures"]],
              ], widths=[2.7*inch, .8*inch, 3.3*inch]),
              Spacer(1, 10),
              p("The fresh brightness variation is already present in ISP Y: ISP-left luma ranged "
                f"{aligned['isp_left']['min']:.2f}-{aligned['isp_left']['max']:.2f}, with "
                f"adjacent RMS change {aligned['isp_left']['adjacent_rms_change']:.2f}. "
                "Both sensors tracked it together (0.993428 correlation) while exposure stayed at "
                "7,999 us, analogue gain at 1.99526x, and digital gain at 1.00237x on every frame. "
                "Rectification preserved source brightness at 0.999996 correlation. All 60 encoded "
                "packets were byte-identical after PC receipt, with zero decode failures. The "
                "remaining physical hypotheses are XTR pulse-width variation and modulated "
                "illumination; a scope/logic-analyzer trace under DC lighting is the next gate.",
                "Callout"), PageBreak()]

    visual_page(story, "Fresh 60-frame flicker trace",
                CURRENT_HIL / "rendered/luma_stages.png",
                "ISP and rectified luma move together while reported exposure and gains stay flat. "
                "The H.264 curves encode disparity, so their different luma scale is expected.")
    visual_page(story, "Fresh dark/bright aligned stage samples",
                CURRENT_HIL / "rendered/dark_vs_bright_stages.png",
                "Rows are the darkest and brightest ISP frames. Pi and PC H.264 disparity frames "
                "are visually and byte-for-byte identical.")

    story += [p("Implemented changes", "H1x"),
              table([
                  ["Priority", "Change", "Verification"],
                  ["P0", "Calibration-bound right rectification-map correction", "3 captures; feature p50 ~1.77 to 0.15 px"],
                  ["P0", "Runtime-selectable 3-way / standard / HH4 / HH", "Backend, strict CLI, UI, focused tests"],
                  ["P0", "Validity-preserving median 0 / 3 / 5", "Mask fixed to original classification; dataset + 3 captures"],
                  ["P0", "Quality, balanced, fast, wide UI presets", "Static UI and configuration contract tests"],
                  ["P1", "Full/half native benchmark with Bad-1/2/3, MAE, RMSE, coverage", "Sintel Clean + Final"],
                  ["P1", "Even scene sampling and half-grid invalid-aware output scoring", "Matrix and self-test"],
                  ["P1", "Explicit horizontal-streak visual/numerical gate", "Full and half, 3 retained captures"],
                  ["P1", "Three independent repeatability runs per final profile", "All visual hashes and non-timing metrics identical"],
                  ["P0", "Colour-gain precision fix in exposure form", "Live submit reloaded daemon; both sensors remained locked"],
                  ["P0", "Deployed native daemon/gateway/tool/UI cohort", "Pi-native CTest 43/43; live raw + encoded controls"],
              ], widths=[.55*inch, 4.65*inch, 3.9*inch], font_size=7.2),
              Spacer(1, 12), p("Remaining hardware gates", "H2x"),
              p("1. Measure XTR low-pulse width with a scope or logic analyzer across an aligned "
                "60-frame run under verified DC illumination. 2. If the pulse is stable, inspect raw "
                "Bayer versus ISP Y and the lighting spectrum/driver. 3. Run the separate advanced "
                "target-calibration process for every real readout/crop/ISP geometry before claiming "
                "metric depth accuracy. 4. Reboot-qualify the final packaged cohort.", "Warn"), PageBreak()]

    matrix_appendix(story, "Appendix A · full-resolution settings matrix",
                    full_rows, "728 × 544")
    matrix_appendix(story, "Appendix B · half-resolution settings matrix",
                    half_rows, "364 × 272 to invalid-aware 728 × 544")

    full_grids = sorted((VALIDATION / "matrix").glob("camera-grid-*.png"))
    half_grids = sorted((VALIDATION / "half-matrix").glob("camera-grid-*.png"))
    for path in full_grids:
        visual_page(story, f"Appendix C · full settings grid · {path.stem}", path,
                    "Single retained camera pair; pre-correction diagnostic grid. Labels include "
                    "coverage, photometric residual, and host matcher time.")
    for path in half_grids:
        visual_page(story, f"Appendix D · half settings grid · {path.stem}", path,
                    "Single retained camera pair; 364 × 272 matching and invalid-aware upscaling. "
                    "This matrix predates the final HH addendum, which is shown in the main report.")

    document = SimpleDocTemplate(str(OUTPUT), pagesize=landscape(letter),
                                 rightMargin=.55*inch, leftMargin=.55*inch,
                                 topMargin=.5*inch, bottomMargin=.55*inch,
                                 title="Nuvo Depth Stereo Validation",
                                 author="Nuvo Depth engineering")
    document.build(story, onFirstPage=on_page, onLaterPages=on_page)
    print(OUTPUT)


if __name__ == "__main__":
    main()
