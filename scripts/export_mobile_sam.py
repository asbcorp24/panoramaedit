"""Export a combined point-prompt MobileSAM graph for PanoramaEdit."""
import argparse
from pathlib import Path
import torch
import onnx
import onnxruntime as ort
import numpy as np
from mobile_sam import sam_model_registry
from mobile_sam.utils.onnx import SamOnnxModel

class Combined(torch.nn.Module):
    def __init__(self, sam):
        super().__init__()
        self.encoder = sam.image_encoder
        self.decoder = SamOnnxModel(sam, return_single_mask=True)
    def forward(self, image, point_coords, point_labels):
        embedding = self.encoder(image)
        # The native caller provides normalized RGB and a positive point.
        # Append SAM's padding point, matching the standard predictor.
        coords = torch.cat((point_coords, torch.zeros_like(point_coords)), dim=1)
        labels = torch.cat((point_labels, -torch.ones_like(point_labels)), dim=1)
        masks, _, _ = self.decoder(
            embedding, coords, labels,
            image.new_zeros((1, 1, 256, 256)),
            image.new_zeros((1,)),
            image.new_tensor([1024., 1024.]))
        return masks

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--checkpoint", required=True)
    p.add_argument("--output", required=True)
    args = p.parse_args()
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(".tmp.onnx")
    sam = sam_model_registry["vit_t"](checkpoint=args.checkpoint).eval()
    model = Combined(sam).eval()
    image = torch.zeros(1, 3, 1024, 1024)
    coords = torch.tensor([[[512., 512.]]])
    labels = torch.ones(1, 1)
    with torch.no_grad():
        torch.onnx.export(model, (image, coords, labels), str(tmp),
            input_names=["image", "point_coords", "point_labels"],
            output_names=["masks"], opset_version=17, dynamo=False,
            do_constant_folding=True)
    onnx.checker.check_model(str(tmp))
    session = ort.InferenceSession(str(tmp), providers=["CPUExecutionProvider"])
    mask = session.run(["masks"], {"image": image.numpy(),
        "point_coords": coords.numpy(), "point_labels": labels.numpy()})[0]
    if mask.shape != (1, 1, 1024, 1024) or not np.isfinite(mask).all():
        raise RuntimeError(f"Unexpected mask: {mask.shape}")
    tmp.replace(out)
    print(f"Validated model: {out}")

if __name__ == "__main__":
    main()
