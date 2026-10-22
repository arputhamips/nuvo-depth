"""Approved pointer selection cannot promote development or draft versions."""
import inspect
import io
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import patch, Mock
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import nuvo_release as release

def reference(**changes):
    return dict(schema_version=1,repository='test/nuvo',approved=True,channel='preview',tag='v0.1.1-beta.6',profiles=['appliance','minimal'])|changes
def published(profile='appliance',**changes):
    name=release.MINIMAL_BUNDLE_NAME if profile=='minimal' else release.BUNDLE_NAME
    return dict(tag_name='v0.1.1-beta.6',draft=False,prerelease=True,published_at='2026-10-21T21:49:11Z',assets=[dict(name=name,size=1024,state='uploaded'),dict(name=name+'.sig',size=64,state='uploaded')])|changes

class ApprovedTest(unittest.TestCase):
    def resolve(self,ref=None,item=None,profile='appliance'):
        opener=Mock();opener.open.side_effect=[io.BytesIO(json.dumps(reference() if ref is None else ref).encode()),io.BytesIO(json.dumps(published(profile) if item is None else item).encode())]
        with patch('urllib.request.build_opener',return_value=opener): result=release.latest_release('test/nuvo',profile)
        self.assertEqual([c.args[0].full_url for c in opener.open.call_args_list],[
            'https://raw.githubusercontent.com/test/nuvo/main/deployment/public-release.json',
            'https://api.github.com/repos/test/nuvo/releases/tags/v0.1.1-beta.6'])
        return result
    def test_only_the_approved_public_version_is_selected(self):
        self.assertEqual(self.resolve(),'v0.1.1-beta.6')
        self.assertEqual(self.resolve(profile='minimal'),'v0.1.1-beta.6')
    def test_draft_wrong_channel_tag_or_incomplete_assets_fail_closed(self):
        for item in [published(draft=True),published(prerelease=False),published(tag_name='v0.1.1-beta.7'),published(assets=[]),published(published_at=None)]:
            with self.subTest(item=item),self.assertRaises(ValueError):self.resolve(item=item)
    def test_invalid_or_unapproved_pointer_never_reaches_installation(self):
        for ref in [reference(approved=False),reference(tag='main'),reference(channel='stable'),reference(profiles=[]),reference(profiles=None),reference(repository='other/repo')]:
            with self.subTest(ref=ref),self.assertRaises(ValueError):self.resolve(ref=ref)
        with patch('urllib.request.build_opener') as opener:
            opener.return_value.open.side_effect=OSError('API unavailable')
            with self.assertRaisesRegex(OSError,'API unavailable'):release.latest_release('test/nuvo')
    def test_standalone_resolvers_match_the_shared_tested_function(self):
        for name in ['install-nuvo-depth.sh','install-nuvo-depth-minimal.sh']:
            text=(ROOT/name).read_text();embedded=text.split('# BEGIN PUBLIC RELEASE RESOLVER\n',1)[1].split('# END PUBLIC RELEASE RESOLVER',1)[0]
            self.assertEqual(embedded,inspect.getsource(release.latest_release))

if __name__=='__main__':unittest.main()
