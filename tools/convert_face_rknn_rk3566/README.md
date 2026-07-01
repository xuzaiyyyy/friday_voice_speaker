# RK3566 face model conversion

This folder is for converting the smart-door-eye face models on an x86_64 server
such as AutoDL. Do not convert from an existing `.rknn` file. Use the original
ONNX/PyTorch/TFLite source model files.

The Qt code expects:

- `RetinaFace.rknn`
  - input: RGB image, 320x320
  - outputs: RetinaFace `loc`, `conf`, `landmark`
- `rec.rknn`
  - input: RGB face crop, 112x112
  - output: 128-d face feature

Use the same RKNN Toolkit2 generation that worked for the Orange Pi runtime.
If the board can run your `toolkit160` emotion model, start with
`rknn-toolkit2==1.6.0`. If it still reports model-version errors, try the
same version family as `/usr/lib/librknnrt.so` on the board.

## Convert

```bash
cd ~/convert_face_rknn_rk3566
python3 -m venv .venv
source .venv/bin/activate

# Install the RKNN Toolkit2 wheel from Rockchip/your existing AutoDL package.
# Example only; use the wheel version that matches the Orange Pi runtime:
# pip install rknn_toolkit2-1.6.0+81f21f4d-cp38-cp38-linux_x86_64.whl

python convert_face_models.py \
  --target rk3566 \
  --det-onnx ./RetinaFace.onnx \
  --rec-onnx ./rec.onnx \
  --out-dir ./out
```

Then copy:

```bash
scp ./out/RetinaFace.rknn ./out/rec.rknn orangepi@<ip>:~/cpp/friday_voice_speaker/desktop/models/
```

## Notes

- If you only have `RetinaFace.rknn` and `rec.rknn` for RK3588, you cannot
  reliably retarget them to RK3566. Find the original ONNX/source models.
- If recognition accuracy changes after conversion, the likely cause is
  different `mean/std` preprocessing. Reuse the original RK3588 conversion
  script and change only `target_platform` from `rk3588` to `rk3566`.
- For a quick first pass, use no calibration dataset. For better NPU accuracy,
  pass `--dataset dataset.txt`, where each line is an image path.
