import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import zipfile

import make_release


class ReleasePackTests(unittest.TestCase):
    def setUp(self):
        contexts = contextlib.ExitStack()
        self.addCleanup(contexts.close)
        self.root = Path(contexts.enter_context(tempfile.TemporaryDirectory()))
        self.dist = self.root / 'dist'
        contexts.enter_context(mock.patch.object(make_release, 'ROOT', str(self.root)))
        contexts.enter_context(mock.patch.object(make_release, 'DIST', str(self.dist)))
        contexts.enter_context(mock.patch.object(make_release, 'git', return_value='test-commit'))
        contexts.enter_context(contextlib.redirect_stdout(io.StringIO()))
        for source in make_release.FILES.values():
            path = self.root / source
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding='utf-8')
        self.cps = self.root / make_release.CPS_FILE
        self.cps.write_text('test CPS', encoding='utf-8')

    def pack(self):
        return make_release.pack('test', str(self.cps), 'test CPS version')

    def snapshot(self):
        return {path.name: path.read_bytes() for path in self.dist.iterdir()}

    def test_release_checksums_match_the_published_assets(self):
        zip_path, cps_path, timezone_path, sums_path = map(Path, self.pack())
        with zipfile.ZipFile(zip_path) as archive:
            self.assertIsNone(archive.testzip())
            self.assertIn('OpenDMR-test/opendmr_BFUV32.bin', archive.namelist())
        with zipfile.ZipFile(timezone_path) as archive:
            self.assertCountEqual(archive.namelist(), ['tzdb.bin', 'README.md'])
        expected = ['%s  %s' % (make_release.sha256(path), path.name)
                    for path in (zip_path, cps_path, timezone_path)]
        self.assertEqual(sums_path.read_text().splitlines(), expected)

    def test_missing_firmware_keeps_the_previous_release(self):
        self.pack()
        previous = self.snapshot()
        (self.root / make_release.FILES['opendmr_BFUV32.bin']).unlink()
        with self.assertRaisesRegex(SystemExit, 'missing: .*opendmr_BFUV32.bin'):
            self.pack()
        self.assertEqual(self.snapshot(), previous)

    def test_checksum_failure_keeps_the_previous_release(self):
        self.pack()
        previous = self.snapshot()
        self.cps.write_text('updated CPS', encoding='utf-8')
        with mock.patch.object(make_release, 'sha256', side_effect=OSError('checksum read failed')):
            with self.assertRaisesRegex(OSError, 'checksum read failed'):
                self.pack()
        self.assertEqual(self.snapshot(), previous)


if __name__ == '__main__':
    unittest.main()
