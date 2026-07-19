# pytest configuration for PicOS E2E tests
"""
End-to-end testing framework for PicOS Simulator.

Usage:
    pytest tests/e2e/ -v
    pytest tests/e2e/ -v --headless
    pytest tests/e2e/ -v --port 7880
    pytest tests/e2e/ -v --simulator-path ./build_sim/picos_simulator
"""

import pytest
import shutil
import subprocess
import time
from pathlib import Path

from picos_simulator import PicosSimulator


def pytest_addoption(parser):
    """Add custom command-line options."""
    parser.addoption(
        "--update-baselines",
        action="store_true",
        default=False,
        help="Update baseline screenshots for visual regression tests",
    )
    parser.addoption(
        "--headless",
        action="store_true",
        default=True,
        help="Run simulator in headless mode (no SDL window)",
    )
    parser.addoption(
        "--port",
        action="store",
        type=int,
        default=0,
        help="TCP port for simulator (0 = auto-assign)",
    )
    parser.addoption(
        "--simulator-path",
        action="store",
        default=str(PicosSimulator.DEFAULT_BINARY),
        help="Path to picos_simulator binary",
    )
    parser.addoption(
        "--sd-card-path",
        action="store",
        default=str(PicosSimulator.DEFAULT_SD_CARD),
        help="Path to SD card directory",
    )


@pytest.fixture(scope="session")
def simulator_binary(request) -> Path:
    """Ensure simulator is built and return path to binary."""
    binary_path = Path(request.config.getoption("--simulator-path"))

    if not binary_path.exists():
        # Auto-build
        try:
            PicosSimulator.build()
        except subprocess.CalledProcessError as e:
            pytest.fail(f"Failed to build simulator: {e}")

    if not binary_path.exists():
        pytest.fail(f"Simulator binary not found: {binary_path}")

    return binary_path


@pytest.fixture(scope="session")
def session_sd_card(request, tmp_path_factory):
    """Session-scoped SD card with default apps + test fixture apps merged."""
    sd_path = tmp_path_factory.mktemp("sd_card")

    # Copy default SD card contents
    default_sd = Path(request.config.getoption("--sd-card-path"))
    if default_sd.exists():
        shutil.copytree(default_sd, sd_path, dirs_exist_ok=True)

    # Ensure required directories exist
    (sd_path / "apps").mkdir(exist_ok=True)
    (sd_path / "data").mkdir(exist_ok=True)
    (sd_path / "system").mkdir(exist_ok=True)

    # Copy test fixture apps
    fixture_apps = Path(__file__).parent / "apps"
    if fixture_apps.exists():
        for app_dir in fixture_apps.iterdir():
            if app_dir.is_dir():
                dest = sd_path / "apps" / app_dir.name
                if not dest.exists():
                    shutil.copytree(app_dir, dest)

    return sd_path


@pytest.fixture(scope="session")
def session_simulator(request, simulator_binary, session_sd_card) -> PicosSimulator:
    """Session-scoped simulator instance (shared across all tests in session).

    Use this for test suites that don't need a fresh simulator per test.
    """
    headless = request.config.getoption("--headless")
    port = request.config.getoption("--port")

    sim = PicosSimulator(
        binary_path=str(simulator_binary),
        sd_card_path=str(session_sd_card),
        headless=headless,
        tcp_port=port,
    )
    sim.start()
    yield sim
    sim.stop()


@pytest.fixture(scope="function")
def test_sd_card(tmp_path, request):
    """Create temporary SD card directory with default + test fixture apps."""
    sd_path = tmp_path / "sd_card"
    sd_path.mkdir()

    # Copy default SD card contents
    default_sd = Path(request.config.getoption("--sd-card-path"))
    if default_sd.exists():
        shutil.copytree(default_sd, sd_path, dirs_exist_ok=True)

    # Ensure required directories
    (sd_path / "apps").mkdir(exist_ok=True)
    (sd_path / "data").mkdir(exist_ok=True)
    (sd_path / "system").mkdir(exist_ok=True)

    # Copy test fixture apps
    fixture_apps = Path(__file__).parent / "apps"
    if fixture_apps.exists():
        for app_dir in fixture_apps.iterdir():
            if app_dir.is_dir():
                dest = sd_path / "apps" / app_dir.name
                if not dest.exists():
                    shutil.copytree(app_dir, dest)

    yield sd_path


@pytest.fixture(scope="function")
def simulator(request, simulator_binary, test_sd_card) -> PicosSimulator:
    """Function-scoped simulator instance (fresh per test).

    Uses a merged SD card with default + test fixture apps.
    """
    headless = request.config.getoption("--headless")
    port = request.config.getoption("--port")

    sim = PicosSimulator(
        binary_path=str(simulator_binary),
        sd_card_path=str(test_sd_card),
        headless=headless,
        tcp_port=port,
    )
    sim.start()
    yield sim
    sim.stop()


@pytest.fixture(autouse=True)
def _check_crash_log(request, simulator):
    """Auto-check for simulator crashes after every test.

    If get_crash_log returns a non-empty crash log, fail the test with
    the crash details. This gives free crash detection across all tests.
    """
    yield
    try:
        result = simulator.call("get_crash_log", timeout=2.0)
        crash = result.get("crash_log") or ""
        if crash:
            pytest.fail(
                f"Simulator crash detected after test:\n{crash}"
            )
    except Exception:
        pass  # Simulator may have already stopped


@pytest.fixture
def update_baselines(request):
    """Check if we should update baseline screenshots."""
    return request.config.getoption("--update-baselines")


@pytest.fixture
def fixtures_dir():
    """Return path to baseline screenshots directory."""
    d = Path("tests/e2e/fixtures")
    d.mkdir(parents=True, exist_ok=True)
    return d
