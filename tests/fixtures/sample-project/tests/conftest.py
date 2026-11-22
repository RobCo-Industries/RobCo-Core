# The three lines a project needs. A project with RobCo-Core in
# external/robco-core uses "../external/robco-core/tests" instead.
import os, sys  # noqa: E401
sys.path.insert(0, os.path.join(os.environ.get("ROBCO_CORE", os.path.join(os.path.dirname(__file__), "../../../..")), "tests"))  # noqa: E501
pytest_plugins = ["harness.plugin"]
