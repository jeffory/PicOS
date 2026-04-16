"""Test basic simulator launch and exit functionality."""

import subprocess
import time
import os
import signal
import pytest


class TestBasicLaunchExit:
    """Test that simulator can launch and exit cleanly."""
    
    def test_simulator_launches_and_exits_cleanly(self):
        """Test that simulator starts up and can exit within timeout."""
        # Start simulator with dummy video driver (headless)
        env = os.environ.copy()
        env["SDL_VIDEODRIVER"] = "dummy"
        
        cmd = [
            "./build_sim/picos_simulator",
            "--sd-card", "./apps"
        ]
        
        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env
        )
        
        try:
            # Wait for simulator to initialize (2 seconds)
            time.sleep(2)
            
            # Check process is still running
            assert process.poll() is None, "Simulator exited prematurely"
            
            # Send SIGTERM to request clean shutdown
            process.send_signal(signal.SIGTERM)
            
            # Wait for clean exit with timeout
            try:
                stdout, stderr = process.communicate(timeout=5)
                exit_code = process.returncode
                
                # Exit code should be 0 (success) or 143 (terminated by SIGTERM)
                assert exit_code in [0, -signal.SIGTERM, 143], \
                    f"Unexpected exit code: {exit_code}"
                
                # Check output indicates clean shutdown
                output = stdout.decode('utf-8', errors='ignore')
                assert "Launcher returned" in output or \
                       "Shut down" in output or \
                       "exited cleanly" in output or \
                       exit_code == 143, \
                    f"Simulator did not shut down cleanly. Output:\n{output[:500]}"
                
            except subprocess.TimeoutExpired:
                process.kill()
                stdout, stderr = process.communicate()
                pytest.fail(
                    "Simulator did not exit within 5 seconds - possible hang. "
                    f"Output:\n{stdout.decode('utf-8', errors='ignore')[:500]}"
                )
                
        finally:
            # Ensure cleanup
            if process.poll() is None:
                process.kill()
                process.wait()
    
    def test_simulator_auto_launch_and_exit(self):
        """Test auto-launching an app and then exiting."""
        env = os.environ.copy()
        env["SDL_VIDEODRIVER"] = "dummy"

        cmd = [
            "./build_sim/picos_simulator",
            "--sd-card", "./apps",
            "--launch", "Hello World"
        ]

        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env
        )

        try:
            # Wait for app to launch (needs more time than launcher alone)
            time.sleep(4)

            # Check process is still running
            assert process.poll() is None, "Simulator exited prematurely"

            # Send SIGTERM
            process.send_signal(signal.SIGTERM)

            # Wait for clean exit (give extra time for app cleanup)
            try:
                stdout, stderr = process.communicate(timeout=8)
                exit_code = process.returncode

                # Should exit cleanly (0, SIGTERM/-15, or 143)
                assert exit_code in [0, -signal.SIGTERM, 143], \
                    f"Unexpected exit code: {exit_code}"

                # Verify app was launched
                output = stdout.decode('utf-8', errors='ignore')
                assert "Auto-launching" in output or \
                       "Launching app" in output, \
                    f"App was not auto-launched. Output:\n{output[:500]}"

            except subprocess.TimeoutExpired:
                process.kill()
                pytest.fail("Simulator hang - did not exit within 8 seconds after SIGTERM")

        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    
    def test_simulator_exit_without_hang(self):
        """Verify simulator doesn't hang on exit (regression test)."""
        # This test specifically verifies the fix for the exit hang issue
        env = os.environ.copy()
        env["SDL_VIDEODRIVER"] = "dummy"
        
        cmd = [
            "./build_sim/picos_simulator",
            "--sd-card", "./apps"
        ]
        
        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env
        )
        
        start_time = time.time()
        
        try:
            # Let it run briefly
            time.sleep(1)
            
            # Request exit
            process.send_signal(signal.SIGTERM)
            
            # Measure exit time
            try:
                process.wait(timeout=5)
                exit_time = time.time() - start_time
                
                # Should exit within 5 seconds total
                assert exit_time < 6, f"Exit took too long: {exit_time:.1f}s"
                
            except subprocess.TimeoutExpired:
                process.kill()
                pytest.fail("EXIT HANG DETECTED: Simulator did not exit within 5s (regression)")
                
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
