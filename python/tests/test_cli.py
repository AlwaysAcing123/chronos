import pytest

from chronos_research.cli import main

ID = "a" * 64
REQUIRED = [
    "--chronos-dataset", "tool", "--features", "f", "--labels", "l", "--splits", "s",
    "--split-artifact", ID, "--output", "o",
]  # fmt: skip


@pytest.mark.parametrize(
    ("arguments", "message"),
    [
        ([], "the following arguments are required: command"),
        (["train"], "invalid choice: 'train'"),
        (["evaluate"], "the following arguments are required"),
        (["evaluate", *REQUIRED, "--output", "other"], "--output may be provided only once"),
        (["evaluate", *REQUIRED, "--seed", "1"], "unrecognized arguments: --seed 1"),
        (
            ["evaluate", *REQUIRED[:-4], "--split-artifact", "ABC", "--output", "o"],
            "must be a 64-digit lowercase ID",
        ),
        (["evaluate", *REQUIRED, "--catalog", "c", "--catalog", "d"], "--catalog may be provided only once"),
        # Abbreviations are refused.
        (["evaluate", *REQUIRED[:-2], "--out", "o"], "the following arguments are required: --output"),
        (["inspect", "--output", "o"], "the following arguments are required: --experiment"),
        (["inspect", "--output", "o", "--experiment", "1234"], "must be a 64-digit lowercase ID"),
        (["inspect", "--output", "", "--experiment", ID], "must not be empty"),
        (["evaluate", *REQUIRED, "--catalog", ""], "must not be empty"),
    ],
)
def test_malformed_command_lines_are_refused_readably(
    arguments: list[str], message: str, capsys: pytest.CaptureFixture[str]
) -> None:
    with pytest.raises(SystemExit) as exit_status:
        main(arguments)
    assert exit_status.value.code == 2
    assert message in capsys.readouterr().err
