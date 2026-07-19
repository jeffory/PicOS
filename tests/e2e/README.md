# PicOS E2E Testing Framework

End-to-end testing framework for PicOS Simulator using pytest.

## Quick Start

```bash
# Install dependencies
pip install -r tests/e2e/requirements.txt

# Run all tests
pytest tests/e2e/ -v

# Run specific test file
pytest tests/e2e/test_launcher.py -v

# Run with HTML report
pytest tests/e2e/ -v --html=report.html --self-contained-html

# Update baseline screenshots
pytest tests/e2e/ -v --update-baselines

# Run in parallel
pytest tests/e2e/ -v -n auto
```

## Test Structure

```
tests/e2e/
├── conftest.py              # pytest configuration and fixtures
├── picos_simulator.py       # Simulator control wrapper
├── display.py               # Screenshot comparison utilities
├── requirements.txt         # Python dependencies
├── apps/                    # Test fixture apps
│   └── display_test/
│       ├── app.json
│       └── main.lua
├── fixtures/                # Baseline screenshots
│   └── display_test/
│       ├── startup.png
│       └── red_screen.png
├── test_launcher.py         # Launcher tests
├── test_display.py          # Display API tests
├── test_input.py            # Input handling tests
└── test_apps/               # App-specific tests
    └── test_calculator.py
```

## Writing Tests

### Basic Test

```python
def test_app_launch(simulator):
    """Test launching an app."""
    # Launch app
    simulator.launch_app("hello")
    simulator.wait_frames(30)
    
    # Verify
    screenshot = simulator.get_screenshot()
    assert screenshot.size == (320, 320)
```

### Screenshot Comparison

```python
def test_visual_regression(simulator, update_baselines):
    """Test against baseline screenshot."""
    simulator.launch_app("display_test")
    simulator.wait_frames(10)
    
    screenshot = simulator.get_screenshot()
    verifier = DisplayVerifier("tests/e2e/fixtures")
    
    # Compare or update baseline
    if update_baselines:
        verifier.save_baseline("display_test", screenshot, "display_test")
    else:
        assert verifier.compare("display_test", screenshot, threshold=0.95)
```

### Performance Testing

```python
def test_fps_requirement(simulator):
    """Test FPS meets minimum requirement."""
    monitor = PerformanceMonitor(simulator)
    monitor.assert_fps(duration_sec=1.0, min_fps=40.0)
```

## CI/CD Integration

Tests automatically run in GitHub Actions on push/PR. See `.github/workflows/e2e.yml`.

## Troubleshooting

### Simulator not starting
- Check that simulator is built: `make simulator`
- Verify SDL2 is installed: `sudo apt-get install libsdl2-dev`

### Tests failing due to screenshots
- Update baselines: `pytest tests/e2e/ --update-baselines`
- Check fixtures in `tests/e2e/fixtures/`

### FPS tests failing
- Close other applications
- Check that simulator is running without debug logging
- Increase tolerance in test if needed
