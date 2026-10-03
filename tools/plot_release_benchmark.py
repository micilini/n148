#!/usr/bin/env python3
"""Export measured rate-distortion curves as a standalone README figure."""
import csv
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from release_benchmark import REPORT, CODECS, WORK

LABELS = {"n148i_v1": "N.148i V1", "n148i_v2": "N.148i V2", "jpeg_turbo": "JPEG Turbo",
          "webp_m4": "WebP M4", "webp_m6": "WebP M6", "webp_m6_historical": "WebP M6 (historical)",
          "jxl_e3": "JPEG XL E3", "jxl_e7": "JPEG XL E7"}
COLORS = ["#999999", "#b32067", "#bd7620", "#3c9cc4", "#163e8c", "#8495ac", "#75a541", "#247650"]


def main():
    rows = list(csv.DictReader((REPORT / "aggregate-curves.csv").open()))
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10, "svg.fonttype": "none"})
    fig, axes = plt.subplots(1, 3, figsize=(14, 4.7))
    for ax, metric, title in zip(axes, ("psnr_y_db", "ssim", "butteraugli"),
                               ("PSNR Y · higher is better", "SSIM · higher is better", "Butteraugli · lower is better")):
        for codec, color in zip(CODECS, COLORS):
            selected = sorted([r for r in rows if r["codec"] == codec and r["metric"] == metric], key=lambda r: int(r["point"]))
            x = [float(r["bpp"]) for r in selected]
            y = [float(r["quality_axis"]) * (-1 if metric == "butteraugli" else 1) for r in selected]
            ax.plot(x, y, color=color, marker="o", markersize=3, linewidth=2.8 if codec == "n148i_v2" else 1.3,
                    linestyle="--" if codec == "webp_m6_historical" else "-", label=LABELS[codec])
        ax.set_title(title, loc="left", fontsize=11, fontweight="bold")
        ax.set_xlabel("Bits per pixel")
        ax.grid(alpha=.18)
        ax.spines[["top", "right"]].set_visible(False)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=4, frameon=False, bbox_to_anchor=(.5, -.02))
    fig.suptitle("N.148i V2 · 130 development images", x=.04, ha="left", fontsize=16, fontweight="bold")
    fig.tight_layout(rect=[0, .16, 1, .92])
    fig.savefig(REPORT / "rate-distortion.svg", bbox_inches="tight", metadata={"Date": None})
    WORK.mkdir(parents=True, exist_ok=True)
    fig.savefig(WORK / "rate-distortion-preview.png", bbox_inches="tight", dpi=140)
    print(REPORT / "rate-distortion.svg")


if __name__ == "__main__": main()
