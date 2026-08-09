# Corner-model validation

Apple's implementation remains private. The shader uses published
reverse-engineered UIBezierPath control points and does not claim private API
or pixel-identical behavior across every Apple platform release.

The continuous-corner approximation is reproducible rather than purely
visual. Run the comparison tool to regenerate the numerical metrics and SVG
overlay:

```bash
python tools/compare_corners.py
```

The generated reference artifacts live in [`../artifacts/`](../artifacts/).
