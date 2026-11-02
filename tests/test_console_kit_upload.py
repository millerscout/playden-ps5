from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))

import console_kit_upload  # noqa: E402


def make_kit(folder):
    folder.mkdir(parents=True)
    (folder / 'manifest.json').write_text('{}')
    return folder


class KitFolderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_takes_the_kit_folder_itself(self):
        kit = make_kit(self.root / 'kit')
        self.assertEqual(console_kit_upload.kit_folder(kit), kit)

    def test_finds_the_kit_inside_the_release_folder(self):
        release = self.root / 'playden-kit-0.12'
        kit = make_kit(release / 'some-kit')
        (release / 'licenses').mkdir()
        self.assertEqual(console_kit_upload.kit_folder(release), kit)

    def test_refuses_a_folder_without_exactly_one_kit(self):
        release = self.root / 'playden-kit-0.12'
        release.mkdir()
        with self.assertRaises(SystemExit):
            console_kit_upload.kit_folder(release)
        make_kit(release / 'a')
        make_kit(release / 'b')
        with self.assertRaises(SystemExit):
            console_kit_upload.kit_folder(release)


class ParkedFolderTests(unittest.TestCase):
    def test_names_the_folder_from_the_git_hash(self):
        manifest = {"runtime": {"artifact_revision": "abc-kit-0.16"}}
        self.assertEqual(console_kit_upload.parked_folder("/data/playden/kits", "playden-kit", manifest),
                         "/data/playden/kits/playden-kit-abc")
        kit_h = {"runtime": {"artifact_revision": "bceed2d-kit-h-r1"}}
        self.assertEqual(console_kit_upload.parked_folder("/data/playden/kits", "playden-kit", kit_h),
                         "/data/playden/kits/playden-kit-bceed2d")

    def test_a_kit_without_a_git_hash_is_not_parked(self):
        self.assertIsNone(console_kit_upload.parked_folder("/data/playden/kits", "playden-kit", {}))
        self.assertIsNone(console_kit_upload.parked_folder("/data/playden/kits", "playden-kit",
                                                           {"runtime": {"artifact_revision": "h-r9"}}))
        self.assertIsNone(console_kit_upload.parked_folder("/data/playden/kits", "playden-kit",
                                                           {"runtime": {"artifact_revision": "AB-kit-0.16"}}))
        self.assertIsNone(console_kit_upload.parked_folder("/data/playden/kits", "playden-kit",
                                                           {"runtime": {"artifact_revision": "../abc-kit-0.16"}}))


if __name__ == '__main__':
    unittest.main()
