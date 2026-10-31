"""Entry point for `python -I run.py ...` (isolated mode leaves this folder off sys.path, so it is added here)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from g923cap.__main__ import main  # noqa: E402

sys.exit(main())
