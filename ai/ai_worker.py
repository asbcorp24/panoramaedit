"""Offline ONNX inference worker for PanoramaEdit.

No network calls. Models, input and output are all local files. The Qt editor
starts this script asynchronously with a venv created by setup_ai_windows.bat.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def process(mode: str, image_path: Path, mask_path: Path, model_path: Path,
            output_path: Path) -> None:
    import numpy as np
    import onnxruntime as ort
    from PIL import Image

    if not model_path.is_file():
        raise FileNotFoundError(f"Local ONNX model not found: {model_path}")
    original = Image.open(image_path).convert("RGB")
    original_size = original.size
    session_options = ort.SessionOptions()
    session_options.enable_mem_pattern = False
    session_options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    available = ort.get_available_providers()
    providers = (["DmlExecutionProvider"] if "DmlExecutionProvider" in available else [])
    providers.append("CPUExecutionProvider")
    session = ort.InferenceSession(str(model_path), sess_options=session_options,
                                   providers=providers)

    if mode == "inpaint":
        # gumka/lama-onnx: fixed 512², input RGB 0..1, mask 0/1,
        # output RGB 0..255. Scale only the selected local crop.
        size = (512, 512)
        color = np.asarray(original.resize(size, Image.Resampling.LANCZOS),
                           dtype=np.float32) / 255.0
        binary_mask = Image.open(mask_path).convert("L").resize(size, Image.Resampling.NEAREST)
        mask = (np.asarray(binary_mask, dtype=np.float32) >= 128).astype(np.float32)
        values = {
            "image": np.ascontiguousarray(color.transpose(2, 0, 1)[None]),
            "mask": np.ascontiguousarray(mask[None, None]),
        }
        output = session.run(None, values)[0]
        rgb = np.asarray(output[0].transpose(1, 2, 0), dtype=np.float32)
    elif mode == "sharpen":
        # NAFNet-GoPro-width32. Models can have dynamic or fixed dimensions;
        # process in multiples of 32 and then restore the original crop size.
        model_input = session.get_inputs()[0]
        shape = model_input.shape
        fixed_h = shape[-2] if isinstance(shape[-2], int) and shape[-2] > 0 else None
        fixed_w = shape[-1] if isinstance(shape[-1], int) and shape[-1] > 0 else None
        is_fp16 = "float16" in model_input.type
        if fixed_w and fixed_h:
            canvas = original.resize((fixed_w, fixed_h), Image.Resampling.LANCZOS)
            array = np.asarray(canvas, dtype=np.float32) / 255.0
        else:
            array = np.asarray(original, dtype=np.float32) / 255.0
            h, w = array.shape[:2]
            pad_h, pad_w = (-h) % 32, (-w) % 32
            if pad_h or pad_w:
                array = np.pad(array, ((0, pad_h), (0, pad_w), (0, 0)), mode="reflect")
        array = np.ascontiguousarray(array.transpose(2, 0, 1)[None],
                                    dtype=np.float16 if is_fp16 else np.float32)
        output = session.run(None, {model_input.name: array})[0]
        rgb = np.asarray(output[0].transpose(1, 2, 0), dtype=np.float32)
        # Typical NAFNet output is 0..1; support 0..255 export variants too.
        if float(np.nanmax(rgb)) <= 2.0:
            rgb *= 255.0
    else:
        raise ValueError(f"Unsupported mode: {mode}")

    rgb = np.nan_to_num(rgb, nan=0.0, posinf=255.0, neginf=0.0)
    result = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), "RGB")
    if result.size != original_size:
        result = result.resize(original_size, Image.Resampling.LANCZOS)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    result.save(output_path, format="PNG")


def main() -> None:
    parser = argparse.ArgumentParser(description="PanoramaEdit offline ONNX restoration worker")
    parser.add_argument("--mode", required=True, choices=("sharpen", "inpaint"))
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--mask", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    process(args.mode, args.image, args.mask, args.model, args.output)


if __name__ == "__main__":
    main()
