#!/usr/bin/env python3
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import statistics


def analyze(path: Path, maximum_rmse_m: float, maximum_bias_m: float,
            minimum_coverage: float) -> dict:
    with path.open(newline="",encoding="utf-8") as source:
        rows=list(csv.DictReader(source))
    if not rows: raise ValueError("accuracy CSV is empty")
    total=len(rows); errors=[]
    for row in rows:
        if row.get("valid","1").lower() not in {"1","true","yes"}: continue
        target=float(row["target_m"]); measured=float(row["measured_m"])
        if math.isfinite(target) and math.isfinite(measured) and target>0 and measured>0: errors.append(measured-target)
    coverage=len(errors)/total
    bias=statistics.fmean(errors) if errors else math.inf
    rmse=math.sqrt(statistics.fmean(error*error for error in errors)) if errors else math.inf
    passed=coverage>=minimum_coverage and rmse<=maximum_rmse_m and abs(bias)<=maximum_bias_m
    return {"schema_version":1,"status":"pass" if passed else "fail","samples":total,
            "valid_samples":len(errors),"coverage":coverage,"bias_m":bias,"rmse_m":rmse,
            "thresholds":{"minimum_coverage":minimum_coverage,
                          "maximum_absolute_bias_m":maximum_bias_m,
                          "maximum_rmse_m":maximum_rmse_m}}


def main() -> None:
    parser=argparse.ArgumentParser(description="Score Nuvo depth accuracy and coverage together")
    parser.add_argument("csv"); parser.add_argument("--maximum-rmse-m",type=float,required=True)
    parser.add_argument("--maximum-bias-m",type=float,required=True); parser.add_argument("--minimum-coverage",type=float,required=True)
    parser.add_argument("--output",required=True); args=parser.parse_args()
    result=analyze(Path(args.csv),args.maximum_rmse_m,args.maximum_bias_m,args.minimum_coverage)
    Path(args.output).write_text(json.dumps(result,sort_keys=True,indent=2)+"\n")
    print(json.dumps(result,sort_keys=True))


if __name__=="__main__": main()
