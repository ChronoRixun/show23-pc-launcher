"""Regression checks for the Windows updater matrix observers."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from update_e2e_window import verify_confirmation, wait_for_taskbar_target


class ObserverTests(unittest.TestCase):
    def test_taskbar_target_can_arrive_after_visibility(self):
        expected = '"fixture/show23-pc.exe"'
        states = iter((None, None, expected))
        self.assertEqual(wait_for_taskbar_target(17, lambda hwnd: next(states)), (expected, 2))

    def test_taskbar_target_must_eventually_be_published(self):
        with self.assertRaisesRegex(AssertionError, "taskbar relaunch command"):
            wait_for_taskbar_target(17, lambda hwnd: None, seconds=.01)

    def test_existing_confirmed_pointer_can_precede_visibility(self):
        version = "0.1.3-beta.1"
        pointer = f"current {version}\ntrial -\nstarts 0\n"
        observations = [{"visible": False, "current": version}, {"visible": True, "current": version}]
        verify_confirmation(pointer, pointer, observations, version, already_current=True)
        with self.assertRaises(AssertionError):
            verify_confirmation(pointer, pointer + "changed", observations, version, already_current=True)
        with self.assertRaises(AssertionError):
            verify_confirmation("", pointer, observations, version, already_current=False)


if __name__ == "__main__":
    unittest.main()
