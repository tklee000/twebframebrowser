import unittest
from PIL import Image
from compare import compare_pixels, backend_proof, capture_metadata_valid


class PixelComparisonRegression(unittest.TestCase):
    def image(self, color):
        return Image.new("RGBA", (2, 2), color)

    def test_exact(self):
        a = self.image((10, 20, 30, 255))
        result, _ = compare_pixels(a, a)
        self.assertEqual(result["status"], "PASS_EXACT")

    def test_one_channel_one_pixel_fails(self):
        a = self.image((10, 20, 30, 255))
        b = a.copy()
        b.putpixel((1, 1), (11, 20, 30, 255))
        result, _ = compare_pixels(a, b)
        self.assertEqual(result["unexplainedPixels"], 1)
        self.assertEqual(result["maxChannelDifference"], 1)

    def test_backend_requires_exact_software_rgba(self):
        gpu = self.image((10, 20, 30, 255))
        cpu = gpu.copy()
        cpu.putpixel((1, 1), (11, 20, 30, 255))
        result, _ = compare_pixels(gpu, cpu, cpu, True)
        self.assertEqual(result["confirmedBackendPixels"], 1)
        self.assertEqual(result["unexplainedPixels"], 0)
        wrong = cpu.copy()
        wrong.putpixel((1, 1), (12, 20, 30, 255))
        result, _ = compare_pixels(gpu, wrong, cpu, True)
        self.assertEqual(result["unexplainedPixels"], 1)

    def test_backend_disabled_without_equal_reference_measurements(self):
        gpu = self.image((10, 20, 30, 255))
        cpu = self.image((11, 20, 30, 255))
        result, _ = compare_pixels(gpu, cpu, cpu, False)
        self.assertEqual(result["unexplainedPixels"], 4)

    def test_different_dimensions_are_not_resized(self):
        result, _ = compare_pixels(self.image((0, 0, 0, 255)), Image.new("RGBA", (3, 2)))
        self.assertEqual(result["status"], "FAIL_DIMENSIONS")

    def test_alpha_difference_fails(self):
        result, _ = compare_pixels(self.image((0, 0, 0, 255)), self.image((0, 0, 0, 254)))
        self.assertEqual(result["unexplainedPixels"], 4)

    def test_software_flag_alone_is_not_backend_proof(self):
        gpu = {"runtime": "154", "softwareRequested": False}
        cpu = {"runtime": "154", "softwareRequested": True}
        self.assertFalse(backend_proof(gpu, cpu, {}, {})[0])
        def graphics(value):
            return {"gpuPage": {"info": {"featureStatus": {"featureStatus": {
                "gpu_compositing": value, "rasterization": value}}}}}
        self.assertFalse(backend_proof(gpu, cpu, graphics("enabled"), graphics("enabled"))[0])
        self.assertTrue(backend_proof(gpu, cpu, graphics("enabled"), graphics("disabled_software"))[0])
        cpu["runtime"] = "155"
        self.assertFalse(backend_proof(gpu, cpu, graphics("enabled"), graphics("disabled_software"))[0])

    def test_dpi_metadata_cannot_be_inferred_from_png_size(self):
        status = {"status": "captured", "dpi": 144, "cssViewport": [800, 600],
                  "pixelSize": [1200, 900], "pageScriptsEnabled": False}
        self.assertTrue(capture_metadata_valid(status, [800, 600], 144))
        status["dpi"] = 96
        self.assertFalse(capture_metadata_valid(status, [800, 600], 144))
        status["dpi"] = 144
        status["cssViewport"] = [1200, 900]
        self.assertFalse(capture_metadata_valid(status, [800, 600], 144))

    def test_capture_script_mode_and_success_are_required(self):
        status = {"status": "captured", "dpi": 96, "cssViewport": [800, 600],
                  "pixelSize": [800, 600], "pageScriptsEnabled": True}
        self.assertFalse(capture_metadata_valid(status, [800, 600], 96))
        status["pageScriptsEnabled"] = False
        status["status"] = "error"
        self.assertFalse(capture_metadata_valid(status, [800, 600], 96))


if __name__ == "__main__":
    unittest.main()
