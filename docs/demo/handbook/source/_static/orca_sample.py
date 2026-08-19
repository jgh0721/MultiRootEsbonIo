"""A sample module used by literalinclude in the handbook."""

from dataclasses import dataclass


@dataclass(frozen=True)
class FrameBudget:
    """How long each stage of a frame is allowed to take, in microseconds."""

    cull: int = 1200
    record: int = 2400
    submit: int = 900

    @property
    def total(self) -> int:
        return self.cull + self.record + self.submit


def pace(budget: FrameBudget, measured: dict[str, int]) -> list[str]:
    """Return the names of the stages that went over budget."""
    over = []
    for stage in ("cull", "record", "submit"):
        if measured.get(stage, 0) > getattr(budget, stage):
            over.append(stage)
    return over


if __name__ == "__main__":
    print(pace(FrameBudget(), {"cull": 1500, "record": 2000, "submit": 100}))
