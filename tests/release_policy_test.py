"""Release channels and human-written changelog gates; no network or publication."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import release_notes


class ReleasePolicyTest(unittest.TestCase):
    def test_beta_and_stable_channels_cannot_be_confused(self):
        for tag, channel in (("v1.2.3-beta.4", "beta"), ("v1.2.3", "stable")):
            release_notes.validate_channel(tag, channel)
        for tag, channel in (("v1.2.3-beta.4", "stable"), ("v1.2.3", "beta"),
                             ("main", "stable"), ("v1.2.3-rc.1", "beta"), ("v1.2.3", "publish")):
            with self.subTest(tag=tag, channel=channel), self.assertRaises(ValueError):
                release_notes.validate_channel(tag, channel)

    def test_only_the_selected_reviewed_version_is_used(self):
        changes = release_notes.changelog_entry((ROOT / "CHANGELOG.md").read_text(encoding="utf-8"), "v0.1.1-beta.4")
        self.assertIn("signed package bundle", changes)
        self.assertNotIn("separate bounded workers", changes)
        self.assertNotIn("browser-normalized", changes)
        with self.assertRaises(ValueError):
            release_notes.changelog_entry("## [Unreleased]\n- Not reviewed.\n", "v1.2.3")

    def test_empty_duplicate_and_paragraph_notes_are_rejected(self):
        for document in ("## [v1.2.3]\n", "## [v1.2.3]\nLong prose.\n",
                         "## [v1.2.3]\n- One.\n## [v1.2.3]\n- Two.\n",
                         "## [v1.2.3]\n- Use --factory.\n",
                         "## [v1.2.3]\n- No GitHub account required.\n"):
            with self.subTest(document=document), self.assertRaises(ValueError):
                release_notes.changelog_entry(document, "v1.2.3")

    def test_render_ends_at_the_pinned_install_command(self):
        result = release_notes.render(ROOT, "arputhamips/nuvo-depth", "v0.1.1-beta.5", "beta")
        self.assertTrue(result.startswith("## Changelog\n"))
        self.assertLess(result.index("### Changed"), result.index("## Install"))
        self.assertIn("Raspberry Pi OS Lite 64-bit", result)
        self.assertIn("releases/download/v0.1.1-beta.5/install-nuvo-depth.sh | sudo bash", result)
        self.assertIn("without a browser password", result)
        self.assertTrue(result.endswith("| sudo bash\n```\n"))
        self.assertNotIn("factory", result.casefold())
        self.assertNotIn("ssh key", result.casefold())

    def test_both_installation_choices_use_the_same_pinned_candidate(self):
        result = release_notes.render(ROOT, "arputhamips/nuvo-depth", "v0.1.1-beta.6", "beta", profiles=("appliance", "minimal"))
        commands = [line for line in result.splitlines() if line.startswith("curl ")]
        prefix = "curl -fsSL https://github.com/arputhamips/nuvo-depth/releases/download/v0.1.1-beta.6/"
        self.assertEqual(commands, [prefix + script + " | sudo bash" for script in ("install-nuvo-depth.sh", "install-nuvo-depth-minimal.sh")])
        self.assertIn("configuration over SSH", result)
        self.assertTrue(result.endswith(commands[-1] + "\n```\n"))
        self.assertNotIn("without a browser password", result)
        self.assertNotIn("factory", result.casefold())
        for profiles in ((), ("minimal", "minimal"), ("unknown",)):
            with self.subTest(profiles=profiles), self.assertRaises(ValueError):
                release_notes.render(ROOT, "arputhamips/nuvo-depth", "v0.1.1-beta.6", "beta", profiles=profiles)


if __name__ == "__main__":
    unittest.main()
