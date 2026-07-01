#!/usr/bin/env python3
import argparse
import os
import sys


def fail(message: str, code: int = 1) -> None:
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(code)


def parse_size(value: str):
    if not value:
        return None
    parts = [int(x) for x in value.replace("x", ",").split(",") if x.strip()]
    if len(parts) not in (3, 4):
        fail(f"invalid input size '{value}', expected C,H,W or N,C,H,W")
    if len(parts) == 3:
        parts = [1] + parts
    return parts


def parse_triplet(value: str):
    parts = [float(x) for x in value.split(",") if x.strip()]
    if len(parts) != 3:
        fail(f"invalid triplet '{value}', expected a,b,c")
    return parts


def call_ok(ret, step: str):
    if ret != 0:
        fail(f"{step} failed, ret={ret}")


def convert_onnx(
    rknn_cls,
    model_path,
    output_path,
    target,
    mean_values,
    std_values,
    dataset,
    input_name,
    input_size,
    quantized_dtype,
    verbose,
):
    if not os.path.isfile(model_path):
        fail(f"model not found: {model_path}")

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    rknn = rknn_cls(verbose=verbose)
    try:
        call_ok(
            rknn.config(
                target_platform=target,
                mean_values=[mean_values],
                std_values=[std_values],
                quantized_dtype=quantized_dtype,
            ),
            f"config {model_path}",
        )

        load_kwargs = {"model": model_path}
        if input_name:
            load_kwargs["inputs"] = [input_name]
        if input_size:
            load_kwargs["input_size_list"] = [input_size]
        call_ok(rknn.load_onnx(**load_kwargs), f"load_onnx {model_path}")

        do_quantization = bool(dataset)
        build_kwargs = {"do_quantization": do_quantization}
        if dataset:
            if not os.path.isfile(dataset):
                fail(f"dataset file not found: {dataset}")
            build_kwargs["dataset"] = dataset
        call_ok(rknn.build(**build_kwargs), f"build {model_path}")
        call_ok(rknn.export_rknn(output_path), f"export {output_path}")
    finally:
        rknn.release()

    print(f"exported: {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Convert RetinaFace/MobileFaceNet ONNX models for RK3566."
    )
    parser.add_argument("--target", default="rk3566", choices=["rk3566", "rk3568"])
    parser.add_argument("--det-onnx", required=True, help="RetinaFace ONNX path")
    parser.add_argument("--rec-onnx", required=True, help="MobileFaceNet/rec ONNX path")
    parser.add_argument("--out-dir", default="out")
    parser.add_argument("--dataset", default="", help="optional calibration image list")
    parser.add_argument("--verbose", action="store_true")
    parser.add_argument("--quantized-dtype", default="asymmetric_quantized-u8")

    parser.add_argument("--det-input-name", default="")
    parser.add_argument("--det-input-size", default="", help="optional N,C,H,W, e.g. 1,3,320,320")
    parser.add_argument("--rec-input-name", default="")
    parser.add_argument("--rec-input-size", default="", help="optional N,C,H,W, e.g. 1,3,112,112")

    parser.add_argument("--det-mean", default="0,0,0")
    parser.add_argument("--det-std", default="1,1,1")
    parser.add_argument("--rec-mean", default="0,0,0")
    parser.add_argument("--rec-std", default="1,1,1")
    args = parser.parse_args()

    try:
        from rknn.api import RKNN
    except Exception as exc:
        fail(f"cannot import rknn.api.RKNN: {exc}")

    det_out = os.path.join(args.out_dir, "RetinaFace.rknn")
    rec_out = os.path.join(args.out_dir, "rec.rknn")

    convert_onnx(
        RKNN,
        args.det_onnx,
        det_out,
        args.target,
        parse_triplet(args.det_mean),
        parse_triplet(args.det_std),
        args.dataset,
        args.det_input_name,
        parse_size(args.det_input_size),
        args.quantized_dtype,
        args.verbose,
    )
    convert_onnx(
        RKNN,
        args.rec_onnx,
        rec_out,
        args.target,
        parse_triplet(args.rec_mean),
        parse_triplet(args.rec_std),
        args.dataset,
        args.rec_input_name,
        parse_size(args.rec_input_size),
        args.quantized_dtype,
        args.verbose,
    )


if __name__ == "__main__":
    main()
