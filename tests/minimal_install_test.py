"""Minimal installer conflict handling and signed profile separation."""
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
import nuvo_minimal_cli as cli
import nuvo_release as release
import build_install_release as builder


class PreflightTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(); self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root/'proc/device-tree').mkdir(parents=True)
        (self.root/'proc/device-tree/model').write_bytes(b'Raspberry Pi 5 Model B\x00')
        (self.root/'etc').mkdir(); (self.root/'etc/os-release').write_text('ID=debian\nVERSION_CODENAME=trixie\n')
        self.calls = []

    def runner(self, args, **kwargs):
        self.calls.append(args)
        return subprocess.CompletedProcess(args, 0, 'arm64\n' if args == ['dpkg','--print-architecture'] else '')

    def test_preflight_requires_no_network_manager_or_wifi_tools(self):
        self.assertEqual(cli.install_preflight(self.root, self.runner, kernel='fixture')['kernel'], 'fixture')
        self.assertTrue(all(args[0] in ('dpkg', 'dpkg-query') for args in self.calls))

    def test_existing_full_install_is_rejected_before_mutation(self):
        def installed(args, **kwargs):
            if args[-1] == 'nuvo-depth-core': return subprocess.CompletedProcess(args, 0, 'installed')
            return self.runner(args, **kwargs)
        with self.assertRaisesRegex(ValueError, 'exclusive'):
            cli.install_preflight(self.root, installed)


@unittest.skipUnless(shutil.which('dpkg-deb') and shutil.which('openssl'), 'Debian fixture tools required')
class SignedMinimalTest(unittest.TestCase):
    def test_signed_minimal_bundle_and_appliance_contract_remain_separate(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); packages = base/'packages'; packages.mkdir()
            tree = base/'package/DEBIAN'; tree.mkdir(parents=True)
            (tree/'control').write_text('Package: nuvo-depth-minimal\nVersion: 1.2.3\nArchitecture: arm64\nMaintainer: Test <test@example.com>\nDescription: fixture\n')
            subprocess.run(['dpkg-deb', '--build', '--root-owner-group', str(tree.parent), str(packages/'minimal.deb')], check=True, capture_output=True)
            key = base/'key'
            subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519', '-out', str(key)], check=True, capture_output=True)
            platform = release.load_json(ROOT/'deployment/release-platform.json')
            assets = base/'assets'
            manifest = builder.build(ROOT, packages, assets, 'test/nuvo', 'v1.2.3', '1.2.3', platform, key, profile='minimal')
            self.assertEqual(manifest['profile'], 'minimal')
            with zipfile.ZipFile(assets/'nuvo-install.pyz') as archive:
                self.assertNotIn('nuvo_setup.py', archive.namelist())
                self.assertIn('nuvo_minimal_cli.py', archive.namelist())
            help_text = subprocess.check_output([sys.executable, str(assets/'nuvo-install.pyz'), '--help'], text=True)
            self.assertIn('--package', help_text); self.assertNotIn('--factory', help_text)
            _, files = release.acquire_release(base, assets/'release.pub', bundle=assets, profile='minimal')
            self.assertEqual(len(files), 1)
            with self.assertRaisesRegex(ValueError, 'profile'):
                release.acquire_release(base, assets/'release.pub', bundle=assets)
            mixed = copy.deepcopy(manifest); mixed.pop('profile')
            with self.assertRaises(ValueError): release.validate_manifest(mixed)
            publication = base/'publication'
            builder.publish_bundle(assets, publication, key)
            self.assertEqual({p.name for p in publication.iterdir()}, {
                release.MINIMAL_BUNDLE_NAME, release.MINIMAL_BUNDLE_NAME+'.sig', 'install-nuvo-depth-minimal.sh'})
            # A shared release can carry both profiles without mixing package
            # cohorts, installers or download names, even from one input directory.
            for package in sorted(release.RUNTIME_PACKAGES):
                (tree/'control').write_text(f'Package: {package}\nVersion: 1.2.3\nArchitecture: arm64\nMaintainer: Test <test@example.com>\nDescription: fixture\n')
                subprocess.run(['dpkg-deb', '--build', '--root-owner-group', str(tree.parent), str(packages/(package+'.deb'))], check=True, capture_output=True)
            full = base/'full'
            full_manifest = builder.build(ROOT, packages, full, 'test/nuvo', 'v1.2.3', '1.2.3', platform, key)
            self.assertEqual({f['package'] for f in full_manifest['files'] if 'package' in f}, release.RUNTIME_PACKAGES)
            full_downloads = base/'full-downloads'
            builder.publish_bundle(full, full_downloads, key)
            names = {p.name for p in publication.iterdir()} | {p.name for p in full_downloads.iterdir()}
            self.assertEqual(len(names), 6)
            for profile, folder, bundle, script in (
                ('appliance', full_downloads, release.BUNDLE_NAME, 'install-nuvo-depth.sh'),
                ('minimal', publication, release.MINIMAL_BUNDLE_NAME, 'install-nuvo-depth-minimal.sh'),
            ):
                release.verify_signature(folder/bundle, folder/(bundle+'.sig'), assets/'release.pub')
                unpacked = base/('unpacked-'+profile)
                release.unpack_bundle(folder/bundle, unpacked)
                cohort, files = release.acquire_release(base, assets/'release.pub', bundle=unpacked, profile=profile)
                self.assertEqual({f['package'] for f in cohort['files'] if 'package' in f}, release.PROFILE_PACKAGES[profile])
                self.assertEqual(len(files), len(release.PROFILE_PACKAGES[profile]))
                self.assertIn('DEFAULT_RELEASE="v1.2.3"', (folder/script).read_text())


if __name__ == '__main__': unittest.main()
