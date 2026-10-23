"""Resolve a package-build ref before scheduling the ARM64 builder."""
import os
from pathlib import Path
import re
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlsplit
from urllib.request import Request, urlopen


def checked_ref(value, fallback):
    value = value or fallback
    if re.fullmatch(r"[0-9a-fA-F]{40}", value):
        return value.lower()
    if re.fullmatch(r"[0-9a-fA-F]{1,39}", value):
        raise ValueError("Shortened source SHA is unsupported; use the full 40-character SHA or an explicit branch/tag ref.")
    # Accept conventional branch/tag names, including qualified refs and slashes.
    # Reject revision expressions, options, whitespace and shell syntax outright.
    if (not value or len(value) > 1024
            or not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_./+-]*", value)
            or ".." in value or "//" in value
            or any(part.startswith('.') or part.endswith(('.', '.lock'))
                   for part in value.split('/'))
            or value.endswith('/')):
        raise ValueError("Invalid source_ref; use a branch, tag or full 40-character SHA without whitespace or revision expressions.")
    if value.startswith('refs/') and not value.startswith(('refs/heads/', 'refs/tags/')):
        raise ValueError("Only branch and tag refs are supported.")
    return value


def resolve_source_ref(value, fallback, repository, api_url='https://api.github.com', token=''):
    ref = checked_ref(value, fallback)
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
        raise ValueError("Invalid GitHub repository.")
    api = urlsplit(api_url)
    if api.scheme != 'https' or not api.netloc or api.username or api.password or api.query or api.fragment:
        raise ValueError("GitHub API URL must be HTTPS without credentials, query or fragment.")
    # The SHA media type avoids downloading an arbitrarily large commit diff.
    request = Request(f"{api_url.rstrip('/')}/repos/{repository}/commits/{quote(ref, safe='')}",
                      headers={'Accept': 'application/vnd.github.sha',
                               'X-GitHub-Api-Version': '2022-11-28',
                               'User-Agent': 'nuvo-package-source-preflight'})
    if token:
        request.add_header('Authorization', 'Bearer ' + token)
    try:
        with urlopen(request, timeout=20) as response:
            payload = response.read(129)
    except HTTPError as error:
        status = error.code
        error.close()
        if status in (404, 422):
            raise ValueError("source_ref does not resolve to a commit in this repository; check the branch/tag or full SHA.") from None
        raise ValueError(f"GitHub source validation failed (HTTP {status}); ARM64 build was not started.") from None
    except (URLError, TimeoutError):
        raise ValueError("GitHub source validation could not complete; ARM64 build was not started. Retry the request.") from None
    if len(payload) > 128 or not re.fullmatch(rb"[0-9a-f]{40}\s*", payload):
        raise ValueError("GitHub returned an invalid commit SHA; ARM64 build was not started.")
    commit = payload.decode('ascii').strip()
    if re.fullmatch(r"[0-9a-f]{40}", ref) and commit != ref:
        raise ValueError("GitHub resolved a different commit than the requested full SHA.")
    return commit


def main():
    try:
        commit = resolve_source_ref(os.environ.get('NUVO_SOURCE_REF', ''),
                                    os.environ.get('NUVO_FALLBACK_SHA', ''),
                                    os.environ['GITHUB_REPOSITORY'],
                                    os.environ.get('GITHUB_API_URL', 'https://api.github.com'),
                                    os.environ.get('GITHUB_TOKEN', ''))
    except ValueError as error:
        print(f"::error title=Package source validation::{error}", file=sys.stderr)
        return 1
    with Path(os.environ['GITHUB_OUTPUT']).open('a', encoding='utf-8') as output:
        output.write(f'source_commit={commit}\n')
    print(f'Resolved immutable package source: {commit}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
