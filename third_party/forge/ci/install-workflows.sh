#!/bin/sh
# Copies the CI workflow into .github/workflows. Run from the repo root,
# then commit and push with your own git login (which has 'workflow' scope).
set -e
mkdir -p .github/workflows
cp ci/github/ci.yml .github/workflows/ci.yml
git add .github/workflows/ci.yml
git commit -m "ci: install GitHub Actions workflow"
echo "Now run: git push origin main"
