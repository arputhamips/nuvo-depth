"""Rolling selection and standalone/source parity; never installs packages."""
import inspect
import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import nuvo_release as release


def published(tag, stamp='2026-10-21T21:49:11Z', *, draft=False, profile='appliance'):
    archive = release.MINIMAL_BUNDLE_NAME if profile == 'minimal' else release.BUNDLE_NAME
    return dict(tag_name=tag, published_at=stamp, draft=draft, prerelease='-' in tag,
                assets=[dict(name=archive, size=1024, state='uploaded'),
                        dict(name=archive + '.sig', size=64, state='uploaded')])


class LatestTest(unittest.TestCase):
    def resolve(self, pages, profile='appliance'):
        responses = [io.BytesIO(json.dumps(page).encode()) for page in pages]
        opener = Mock()
        opener.open.side_effect = responses
        with patch('urllib.request.build_opener', return_value=opener):
            result = release.latest_release('test/nuvo', profile)
        self.assertTrue(opener.open.call_args.args[0].full_url.startswith('https://api.github.com/repos/test/nuvo/releases?'))
        return result

    def test_public_preview_newest_published_date_not_list_order_or_drafts(self):
        items = [published('v99.0.0', draft=True), published('v0.1.1-beta.6'),
                 published('v0.1.1-beta.5', '2026-10-20T00:00:00Z')]
        self.assertEqual(self.resolve([items]), 'v0.1.1-beta.6')

    def test_profile_and_incomplete_uploads(self):
        full = published('v1.0.0', '2026-10-22T00:00:00Z')
        minimal = published('v0.9.0', profile='minimal')
        self.assertEqual(self.resolve([[full, minimal]], 'minimal'), 'v0.9.0')
        full['assets'][1]['size'] = 12
        with self.assertRaisesRegex(ValueError, 'no public'):
            self.resolve([[full]])
        with self.assertRaises(ValueError):
            self.resolve([[published('main')]])

    def test_pagination_and_bad_metadata(self):
        page = [published('v1.0.0', draft=True) for _ in range(100)]
        self.assertEqual(self.resolve([page, [published('v1.0.1')]]), 'v1.0.1')
        with self.assertRaisesRegex(ValueError, 'too many'):
            self.resolve([page] * 10)
        with self.assertRaises(ValueError):
            self.resolve([{'message': 'rate limit'}])
        with patch('urllib.request.build_opener') as opener:
            opener.return_value.open.side_effect = OSError('API unavailable')
            with self.assertRaisesRegex(OSError, 'API unavailable'):
                release.latest_release('test/nuvo')

    def test_shells_embed_the_exact_tested_resolver(self):
        for name in ('install-nuvo-depth.sh', 'install-nuvo-depth-minimal.sh'):
            source = (ROOT/name).read_text()
            embedded = source.split('# BEGIN PUBLIC RELEASE RESOLVER\n', 1)[1].split('# END PUBLIC RELEASE RESOLVER', 1)[0]
            self.assertEqual(embedded, inspect.getsource(release.latest_release))
            self.assertIn('DEFAULT_RELEASE="latest"', source)
            self.assertIn('print(manifest[\'tag\'])', source)


if __name__ == '__main__':
    unittest.main()
