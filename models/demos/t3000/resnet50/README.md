---

# ResNet50 Demo

## Introduction
ResNet50 is a deep convolutional neural network architecture with 50 layers, designed to enable training of very deep networks by using residual learning to mitigate the vanishing gradient problem.

## Details

+ The entry point to the Metal ResNet model is `ResNet` in `ttnn_functional_resnet50.py`.
+ The model picks up certain configs and weights from TorchVision pretrained model. We have used `torchvision.models.ResNet50_Weights.IMAGENET1K_V1` version from TorchVision as our reference.
+ Our ImageProcessor on the other hand is based on `microsoft/resnet-50` from huggingface.

## Performance

### T3000
#### End-to-End Performance
+ For end-to-end performance, run
```python
  WH_ARCH_YAML=wormhole_b0_80_arch_eth_dispatch.yaml pytest models/demos/t3000/resnet50/tests/test_perf_e2e_resnet50.py::test_perf_trace_2cqs
```
+ This will generate a CSV with the timings and throughputs.
+ **Expected end-to-end perf**: For batch = 16 per device, or batch 128 in total, it is about `35,800 fps` currently. This may vary machine to machine.
