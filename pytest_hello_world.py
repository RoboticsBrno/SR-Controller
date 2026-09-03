import logging

import pytest
from pytest_embedded import Dut


@pytest.mark.esp32s3
def test_sender_starts(dut: Dut) -> None:
    dut.expect_exact("Sender ready:", timeout=15)
    logging.info("SR controller sender initialized")
