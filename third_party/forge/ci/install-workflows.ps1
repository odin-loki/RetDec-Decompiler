# Windows version of install-workflows.sh
New-Item -ItemType Directory -Force .github/workflows | Out-Null
Copy-Item ci/github/ci.yml .github/workflows/ci.yml
git add .github/workflows/ci.yml
git commit -m "ci: install GitHub Actions workflow"
Write-Host "Now run: git push origin main"
