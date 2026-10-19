"""Exercise exports and package staging with realistic local junk present."""
from pathlib import Path
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

ROOT = Path(__file__).resolve().parents[1]
RELEASE = ROOT / 'tools/release'
sys.path.insert(0, str(RELEASE))
from nuvo_source_files import source_files


@unittest.skipUnless(shutil.which('git'), 'Git is required for export fixtures')
class ExportTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / 'source'
        self.root.mkdir()
        self.git('init', '-q')
        self.write('.gitignore', '.env\n.codex-tmp/\noutput/\ntmp/\ndata/\nnode_modules/\n')
        self.write('README.md', 'tracked source')
        self.write('tools/build_install_release.py', 'required release builder')
        self.write('install.sh', '#!/bin/sh\necho fixture\n')
        self.git('add', '.')
        self.git('update-index', '--chmod=+x', 'install.sh')
        for name in ('.env', '.codex-tmp/private.json', 'output/capture.bin',
                     'tmp/old.zip', 'data/frame.npy', 'node_modules/dependency.js',
                     'untracked-notes.txt'):
            self.write(name, 'LOCAL-ONLY-CANARY')

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def git(self, *arguments):
        return subprocess.check_output(['git', '-C', str(self.root), *arguments], stderr=subprocess.STDOUT)

    def run_tool(self, name, arguments, *, success=True):
        result = subprocess.run([sys.executable, str(RELEASE/name), *map(str, arguments)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stderr)
        return result

    def test_source_zip_uses_tracked_dirty_content_and_real_modes(self):
        self.write('README.md', 'edited tracked source')
        output = self.base/'source.zip'
        self.run_tool('create_source_bundle.py', ['--source', self.root, '--output', output])
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(set(archive.namelist()), {
                'nuvo-depth/.gitignore', 'nuvo-depth/README.md', 'nuvo-depth/install.sh',
                'nuvo-depth/tools/build_install_release.py', 'nuvo-depth/SOURCE-MANIFEST.json'})
            self.assertEqual(archive.read('nuvo-depth/README.md'), b'edited tracked source')
            self.assertEqual(archive.getinfo('nuvo-depth/install.sh').external_attr >> 16, 0o100755)
            self.assertNotIn(b'LOCAL-ONLY-CANARY', b''.join(archive.read(name) for name in archive.namelist()))

    def test_spdx_has_the_same_source_boundary(self):
        output = self.base/'source.spdx.json'
        self.run_tool('generate_spdx.py', [self.root, output, '--version', 'fixture'])
        files = json.loads(output.read_text())['files']
        self.assertEqual({file['fileName'] for file in files}, {
            './.gitignore', './README.md', './install.sh', './tools/build_install_release.py'})
        builder = next(file for file in files if file['fileName'].endswith('build_install_release.py'))
        self.assertEqual(builder['checksums'][0]['checksumValue'], hashlib.sha256(b'required release builder').hexdigest())

    def test_non_git_source_is_refused_and_staged_inventory_is_explicit(self):
        stage = self.base/'stage'; stage.mkdir()
        (stage/'build-record.json').write_text('intentional distribution file')
        output = self.base/'stage.spdx.json'
        self.run_tool('generate_spdx.py', [stage, output, '--version', 'fixture'], success=False)
        self.assertFalse(output.exists())
        self.run_tool('create_source_bundle.py', ['--source', stage, '--output', self.base/'bad.zip'], success=False)
        self.assertFalse((self.base/'bad.zip').exists())
        self.run_tool('generate_spdx.py', [stage, output, '--version', 'fixture', '--staged-tree'])
        self.assertEqual(json.loads(output.read_text())['files'][0]['fileName'], './build-record.json')

    @unittest.skipUnless(os.name == 'posix', 'POSIX symlink fixture')
    def test_replaced_source_symlink_cannot_read_a_private_target(self):
        private = self.base/'private'; private.write_text('OUTSIDE-ROOT-CANARY')
        (self.root/'README.md').unlink()
        (self.root/'README.md').symlink_to(private)
        for tool, args, output in (
            ('create_source_bundle.py', ['--source', self.root, '--output'], self.base/'bad.zip'),
            ('generate_spdx.py', [self.root], self.base/'bad.spdx.json')):
            arguments = [*args, output] + (['--version','fixture'] if tool == 'generate_spdx.py' else [])
            result = self.run_tool(tool, arguments, success=False)
            self.assertFalse(output.exists())
            self.assertNotIn('OUTSIDE-ROOT-CANARY', result.stdout+result.stderr)

    @unittest.skipUnless(os.name == 'posix', 'POSIX symlink fixture')
    def test_tracked_symlink_is_exported_without_dereferencing(self):
        (self.root/'link').symlink_to('../outside-target')
        self.git('add', 'link')
        output = self.base/'symlink.zip'
        self.run_tool('create_source_bundle.py', ['--source', self.root, '--output', output])
        with zipfile.ZipFile(output) as archive:
            self.assertEqual(archive.read('nuvo-depth/link'), b'../outside-target')
            self.assertEqual(archive.getinfo('nuvo-depth/link').external_attr >> 16, 0o120777)

    def test_spdx_cannot_overwrite_tracked_source(self):
        self.run_tool('generate_spdx.py', [self.root, self.root/'README.md', '--version','fixture'], success=False)
        self.assertEqual((self.root/'README.md').read_text(), 'tracked source')


@unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'), 'CMake and Ninja required')
class PackageTest(unittest.TestCase):
    def test_real_install_ignores_stray_files_and_keeps_referenced_assets(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory); source = base/'source'; source.mkdir()
            # A Git checkout uses its index. A released source archive is already
            # a prepared tree and has no .git directory.
            # Debhelper isolates HOME, hiding the runner's global Git trust.
            # Trust only this known test checkout, scoped to fixture discovery.
            with mock.patch.dict(os.environ, {
                'GIT_CONFIG_COUNT': '1', 'GIT_CONFIG_KEY_0': 'safe.directory',
                'GIT_CONFIG_VALUE_0': str(ROOT),
            }):
                inputs = list(source_files(ROOT, staged_tree=not (ROOT/'.git').exists()))
            for name, data, mode in inputs:
                path = source/name; path.parent.mkdir(parents=True, exist_ok=True)
                if mode & 0o170000 == 0o120000:
                    path.symlink_to(os.fsdecode(data))
                else:
                    path.write_bytes(data)
            for name in ('web/.env', 'web/pipeline-canvas.js', 'web/node_modules/canary.txt',
                         'web/assets/benchmark/unused-secret.jpg',
                         'sdk/python/nuvo_depth/private_notes.py',
                         'presets/unapproved.yaml', 'qualification/private.json'):
                path = source/name; path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('MUST-NOT-SHIP-CANARY')
            build = source/'build/minimal-host'; stage = base/'installed'
            subprocess.run(['cmake','--preset','minimal-host'], cwd=source,
                           stdout=subprocess.DEVNULL, check=True)
            for component in ('device-web','python-sdk','device-data'):
                subprocess.run(['cmake','--install',str(build),'--prefix',str(stage),'--component',component],
                               stdout=subprocess.DEVNULL, check=True)
            installed = [path for path in stage.rglob('*') if path.is_file()]
            self.assertTrue(installed)
            self.assertTrue(all(b'MUST-NOT-SHIP-CANARY' not in path.read_bytes() for path in installed))
            web = stage/'share/nuvo-depth/web'
            html = (web/'index.html').read_text()
            for asset in re.findall(r'(?:href|src)="(/[^"#]+)"', html):
                self.assertTrue((web/asset.lstrip('/')).is_file(), asset)
            self.assertTrue((stage/'lib/python3/dist-packages/nuvo_depth/client.py').is_file())
            subprocess.run([sys.executable, '-I', '-c',
                            'import sys; sys.path.insert(0, sys.argv[1]); '
                            'from nuvo_depth import NuvoClient, decode_frame',
                            str(stage/'lib/python3/dist-packages')], check=True)
            self.assertTrue((stage/'share/nuvo-depth/presets/balanced-728x544-10.yaml').is_file())
            self.assertTrue((stage/'share/nuvo-depth/qualification/pi5-beta.yaml').is_file())
            self.assertFalse(any(path.name.startswith('pipeline') for path in installed))


if __name__ == '__main__':
    unittest.main()
