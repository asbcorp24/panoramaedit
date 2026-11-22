"""Offline ONNX inference worker for PanoramaEdit.

No network calls. Models, input and output are all local files. The Qt editor
starts this script asynchronously with a venv created by setup_ai_windows.bat.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def create_session(model_path: Path, validate_only: bool = False):
    import onnxruntime as ort

    if not model_path.is_file():
        raise FileNotFoundError(f"Не найдена локальная модель: {model_path}")
    options = ort.SessionOptions()
    options.enable_mem_pattern = False
    options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    available = ort.get_available_providers()
    providers = ("CPUExecutionProvider",) if validate_only else tuple(
        (["DmlExecutionProvider"] if "DmlExecutionProvider" in available else [])
        + ["CPUExecutionProvider"])
    try:
        return ort.InferenceSession(str(model_path), sess_options=options,
                                    providers=list(providers))
    except Exception as exc:
        reason = str(exc)
        if "tensor(float16)" in reason and "tensor(float)" in reason:
            raise RuntimeError(
                "Несовместимая FP16-модель (смешаны float32/float16). "
                "Запустите обновлённый setup_ai_windows.bat: он заменит deblur.onnx "
                "на проверенную FP32-версию. " + reason) from exc
        raise


def validate_model(mode: str, model_path: Path) -> None:
    session = create_session(model_path, validate_only=True)
    inputs = session.get_inputs()
    if mode == "sharpen":
        if len(inputs) != 1 or len(inputs[0].shape) != 4:
            raise ValueError("NAFNet: ожидался один четырёхмерный RGB-тензор")
    elif mode == "inpaint":
        if not {"image", "mask"}.issubset({inp.name for inp in inputs}):
            raise ValueError("LaMa: отсутствуют входы image/mask")
    else:
        raise ValueError(f"Неизвестный режим: {mode}")
    print(f"OK: {mode}, {model_path.name}, ONNX session loaded")


def process(mode: str, image_path: Path, mask_path: Path, model_path: Path,
            output_path: Path) -> None:
    import numpy as np
    from PIL import Image

    original = Image.open(image_path).convert("RGB")
    original_size = original.size
    session = create_session(model_path)

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
    parser.add_argument("--image", type=Path)
    parser.add_argument("--mask", type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--validate-model", action="store_true",
                        help="Check ONNX model loading without opening an image")
    args = parser.parse_args()
    try:
        if args.validate_model:
            validate_model(args.mode, args.model)
        else:
            if not all((args.image, args.mask, args.output)):
                parser.error("--image, --mask and --output are required for inference")
            process(args.mode, args.image, args.mask, args.model, args.output)
    except Exception as exc:
        print(f"Ошибка локальной ИИ-модели: {exc}", file=sys.stderr)
        raise SystemExit(1) from None


if __name__ == "__main__":
    main()
