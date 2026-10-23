import importlib.util
from pathlib import Path
import unittest
from contextlib import redirect_stdout, redirect_stderr
import io
import os
import tempfile
from urllib.error import HTTPError, URLError
from unittest import mock
spec=importlib.util.spec_from_file_location('routing',Path(__file__).resolve().parents[1]/'tools/ci/required_checks.py')
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
source_spec=importlib.util.spec_from_file_location('source_ref',Path(__file__).resolve().parents[1]/'tools/ci/resolve_source_ref.py')
source_ref=importlib.util.module_from_spec(source_spec); source_spec.loader.exec_module(source_ref)


class SourceRefTest(unittest.TestCase):
    commit='1234567890abcdef1234567890abcdef12345678'

    def resolve(self, value, payload=None):
        response=mock.MagicMock()
        response.__enter__.return_value.read.return_value=payload if payload is not None else self.commit.encode()+b'\n'
        with mock.patch.object(source_ref,'urlopen',return_value=response) as opened:
            result=source_ref.resolve_source_ref(value,self.commit,'test-owner/nuvo',token='private-fixture-token')
        return result,opened

    def test_full_sha_and_empty_fallback_resolve_immutable_source(self):
        for value in (self.commit,self.commit.upper(),''):
            result,opened=self.resolve(value)
            self.assertEqual(result,self.commit)
            request=opened.call_args.args[0]
            self.assertEqual(request.full_url,'https://api.github.com/repos/test-owner/nuvo/commits/'+self.commit)
            self.assertEqual(request.get_header('Accept'),'application/vnd.github.sha')
            self.assertEqual(opened.call_args.kwargs['timeout'],20)

    def test_branch_tag_and_slash_are_urlencoded(self):
        for value,path in [('main','main'),('v0.1.1-beta.8','v0.1.1-beta.8'),
                           ('feature/ci-fix','feature%2Fci-fix'),('refs/tags/v1.2.3','refs%2Ftags%2Fv1.2.3'),
                           ('refs/heads/deadbeef','refs%2Fheads%2Fdeadbeef')]:
            result,opened=self.resolve(value)
            self.assertEqual(result,self.commit)
            self.assertTrue(opened.call_args.args[0].full_url.endswith('/commits/'+path))

    def test_short_hex_is_rejected_before_remote_request(self):
        with mock.patch.object(source_ref,'urlopen') as opened:
            for value in ('1ea4d8b','deadbeef','a'*39):
                with self.subTest(value=value),self.assertRaisesRegex(ValueError,'full 40-character'):
                    source_ref.resolve_source_ref(value,self.commit,'test-owner/nuvo')
            opened.assert_not_called()

    def test_whitespace_revision_expressions_and_shell_syntax_never_execute(self):
        with mock.patch.object(source_ref,'urlopen') as opened:
            for value in (' ',' main','main\n','main branch','HEAD~1','main^{commit}',
                          '--help','feature//fix','branch.lock','refs/pull/1/head',
                          '$(touch SHOULD_NOT_EXIST)','main;echo nope','main`id`','main|cat'):
                with self.subTest(value=value),self.assertRaises(ValueError):
                    source_ref.resolve_source_ref(value,self.commit,'test-owner/nuvo')
            opened.assert_not_called()

    def test_nonexistent_remote_ref_fails_before_builder(self):
        for status in (404,422):
            with mock.patch.object(source_ref,'urlopen',side_effect=HTTPError('https://fixture',status,'missing',{},None)):
                with self.assertRaisesRegex(ValueError,'does not resolve'):
                    source_ref.resolve_source_ref('no-such-branch',self.commit,'test-owner/nuvo')

    def test_network_and_api_failure_are_explicit(self):
        for error in (URLError('private internal detail'),TimeoutError(),HTTPError('https://fixture',403,'private detail',{},None)):
            with mock.patch.object(source_ref,'urlopen',side_effect=error):
                with self.assertRaisesRegex(ValueError,'ARM64 build was not started'):
                    source_ref.resolve_source_ref('main',self.commit,'test-owner/nuvo')

    def test_invalid_remote_payload_or_different_sha_never_becomes_output(self):
        for payload in (b'bad',b'a'*129,b'0'*40,b'not a commit\nsource_commit=bad'):
            with self.subTest(payload=payload),self.assertRaises(ValueError):
                self.resolve(self.commit,payload)

    def test_only_validated_sha_is_exported(self):
        with tempfile.TemporaryDirectory() as temporary:
            output=Path(temporary)/'output'
            env={'NUVO_SOURCE_REF':'main','NUVO_FALLBACK_SHA':self.commit,'GITHUB_REPOSITORY':'test-owner/nuvo',
                 'GITHUB_OUTPUT':str(output)}
            with mock.patch.dict(os.environ,env),mock.patch.object(source_ref,'resolve_source_ref',return_value=self.commit),redirect_stdout(io.StringIO()):
                self.assertEqual(source_ref.main(),0)
            self.assertEqual(output.read_text(),'source_commit='+self.commit+'\n')

    def test_invalid_input_never_exports_or_discloses_token(self):
        with tempfile.TemporaryDirectory() as temporary:
            output=Path(temporary)/'output'
            error=io.StringIO()
            env={'NUVO_SOURCE_REF':'main;echo injected','NUVO_FALLBACK_SHA':self.commit,
                 'GITHUB_REPOSITORY':'test-owner/nuvo','GITHUB_OUTPUT':str(output),
                 'GITHUB_TOKEN':'private-fixture-token'}
            with mock.patch.dict(os.environ,env),mock.patch.object(source_ref,'urlopen') as opened,redirect_stderr(error):
                self.assertEqual(source_ref.main(),1)
            opened.assert_not_called()
            self.assertFalse(output.exists())
            self.assertIn('Invalid source_ref',error.getvalue())
            self.assertNotIn('private-fixture-token',error.getvalue())
class RoutingTest(unittest.TestCase):
    def test_hosted_ubuntu_mirror_list_indirection_is_rewritten(self):
        script=(Path(__file__).resolve().parents[1]/'tools/ci/use-ubuntu-mirror.sh').read_text()
        body=script.split("<<'PY'\n",1)[1].rsplit('\nPY',1)[0]
        names=('etc/apt/sources.list','etc/apt/sources.list.d/ubuntu.sources',
               'etc/apt/apt-mirrors.txt','etc/apt/apt-mirrors-security.txt')
        original='http://azure.archive.ubuntu.com/ubuntu priority:1\nhttps://azure.archive.ubuntu.com/ubuntu priority:2\nSigned-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg\n'
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            for name in names:
                path=root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(original)
            with mock.patch('pathlib.Path',lambda name:root/name.lstrip('/')),redirect_stdout(io.StringIO()):
                exec(compile(body,'hosted-mirror-fixture','exec'),{})
            for name in names:
                text=(root/name).read_text()
                self.assertNotIn('azure.archive.ubuntu.com',text)
                self.assertIn('https://archive.ubuntu.com/ubuntu',text)
                self.assertIn('Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg',text)
    def test_documentation_stays_light(self):
        self.assertEqual(module.classify(['README.md','docs/install.md','deployment/public-release.json']),{'native':False,'browser':False,'docs_only':True})
    def test_native_sdk_ui_and_calibration_tests_require_runtime_coverage(self):
        for path in ('src/contract.cpp','CMakeLists.txt','include/nuvo/depth.h','sdk/python/nuvo_depth/client.py',
                     'web/app.js','tests/calibration_readiness_test.py','debian/nuvo-depth-minimal.preinst'):
            with self.subTest(path=path): self.assertTrue(module.classify([path])['native'])
    def test_unknown_or_mixed_changes_fail_safe(self):
        self.assertTrue(module.classify([])['browser'])
        self.assertTrue(module.classify(['docs/install.md','unknown-file'])['native'])
        self.assertTrue(module.classify(['docs/example.py'])['native'])
        self.assertTrue(module.classify(['docs/check.sh'])['browser'])
if __name__=='__main__': unittest.main()
