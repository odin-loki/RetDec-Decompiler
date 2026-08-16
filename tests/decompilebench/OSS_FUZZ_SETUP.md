# OSS-Fuzz DecompileBench — not used in this fork

The paper corpus (arXiv 2505.11340, ~23k OSS-Fuzz functions) is **out of scope**.

Stock RetDec 5.0 compare on the **216-binary stand-in** uses `remnux/retdec`.
That is not the OSS-Fuzz paper setup.

```powershell
py -3 scripts\run_stock_retdec_docker.py --profile full --skip-pull
```

See [docs/internal/MAINTAINER_SCOPE.md](../../docs/internal/MAINTAINER_SCOPE.md).
