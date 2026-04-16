"""Display verification utilities for screenshot comparison."""

import numpy as np
from PIL import Image
from pathlib import Path
from typing import Optional, Tuple


class DisplayVerifier:
    """Tools for verifying display output via screenshot comparison."""
    
    def __init__(self, fixtures_dir: str = "tests/e2e/fixtures"):
        self.fixtures_dir = Path(fixtures_dir)
        self.fixtures_dir.mkdir(parents=True, exist_ok=True)
        
    def save_baseline(self, name: str, image: Image.Image, subdir: Optional[str] = None):
        """Save current image as baseline for future comparisons.
        
        Args:
            name: Baseline identifier
            image: Screenshot to save
            subdir: Optional subdirectory within fixtures
        """
        if subdir:
            path = self.fixtures_dir / subdir
            path.mkdir(exist_ok=True)
        else:
            path = self.fixtures_dir
        
        filepath = path / f"{name}.png"
        image.save(filepath)
        return filepath
        
    def compare(self, name: str, image: Image.Image, 
                threshold: float = 0.99, subdir: Optional[str] = None) -> bool:
        """Compare image to baseline with pixel-level similarity.
        
        Args:
            name: Baseline identifier
            image: Current screenshot
            threshold: Minimum pixel match ratio (0-1)
            subdir: Optional subdirectory within fixtures
            
        Returns:
            True if images match within threshold
        """
        if subdir:
            baseline_path = self.fixtures_dir / subdir / f"{name}.png"
        else:
            baseline_path = self.fixtures_dir / f"{name}.png"
        
        if not baseline_path.exists():
            # Auto-generate baseline on first run
            self.save_baseline(name, image, subdir)
            return True
            
        baseline = Image.open(baseline_path)
        
        # Convert to arrays for comparison
        baseline_arr = np.array(baseline)
        image_arr = np.array(image)
        
        if baseline_arr.shape != image_arr.shape:
            return False
            
        # Calculate pixel match ratio
        matching = np.sum(np.all(baseline_arr == image_arr, axis=-1))
        total_pixels = baseline_arr.shape[0] * baseline_arr.shape[1]
        similarity = matching / total_pixels
        
        return similarity >= threshold
        
    def get_pixel(self, image: Image.Image, x: int, y: int) -> Tuple[int, int, int]:
        """Get RGB value at specific pixel.
        
        Args:
            image: Screenshot
            x, y: Pixel coordinates
            
        Returns:
            (R, G, B) tuple
        """
        return image.getpixel((x, y))
        
    @staticmethod
    def _to_rgb(image: Image.Image) -> np.ndarray:
        """Convert image to RGB numpy array (strip alpha if present)."""
        arr = np.array(image)
        if arr.ndim == 3 and arr.shape[2] == 4:
            arr = arr[:, :, :3]
        return arr

    def is_color(self, image: Image.Image, color: Tuple[int, int, int],
                 tolerance: int = 5, region: Optional[Tuple[int, int, int, int]] = None) -> bool:
        """Check if image (or region) is approximately a single color.

        Args:
            image: Screenshot
            color: Expected (R, G, B) color
            tolerance: Maximum per-channel difference
            region: Optional (x, y, w, h) region to check

        Returns:
            True if all pixels in region match color within tolerance
        """
        arr = self._to_rgb(image)

        if region:
            x, y, w, h = region
            arr = arr[y:y+h, x:x+w]

        color_arr = np.array(color)
        diff = np.abs(arr.astype(float) - color_arr)

        return np.all(diff <= tolerance)
        
    def has_color_anywhere(self, image: Image.Image,
                          color: Tuple[int, int, int],
                          tolerance: int = 5) -> bool:
        """Check if color appears anywhere in image."""
        arr = self._to_rgb(image)
        color_arr = np.array(color)
        diff = np.abs(arr.astype(float) - color_arr)
        return np.any(np.all(diff <= tolerance, axis=-1))

    def count_pixels_of_color(self, image: Image.Image,
                             color: Tuple[int, int, int],
                             tolerance: int = 5) -> int:
        """Count pixels matching a specific color."""
        arr = self._to_rgb(image)
        color_arr = np.array(color)
        diff = np.abs(arr.astype(float) - color_arr)
        return int(np.sum(np.all(diff <= tolerance, axis=-1)))


class PerformanceMonitor:
    """Monitor FPS and performance metrics."""
    
    def __init__(self, simulator):
        self.simulator = simulator
        
    def measure_fps(self, duration_sec: float = 1.0, min_fps: float = 40.0) -> dict:
        """Measure FPS and validate it meets minimum requirements.
        
        Args:
            duration_sec: How long to measure
            min_fps: Minimum acceptable FPS (default: 40)
            
        Returns:
            Dict with fps, min_fps, passed
        """
        fps = self.simulator.measure_fps(duration_sec)
        
        return {
            "fps": fps,
            "min_fps": min_fps,
            "passed": fps >= min_fps,
            "duration": duration_sec
        }
        
    def assert_fps(self, duration_sec: float = 1.0, min_fps: float = 40.0):
        """Assert that FPS meets minimum requirement.
        
        Raises:
            AssertionError: If FPS is below minimum
        """
        result = self.measure_fps(duration_sec, min_fps)
        if not result["passed"]:
            raise AssertionError(
                f"FPS {result['fps']:.1f} below minimum {min_fps}"
            )
