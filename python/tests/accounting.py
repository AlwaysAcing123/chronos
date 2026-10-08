"""An independent recomputation of a strategy run's accounting from its
journal alone, in exact fractions, for tests and the review gates. It
shares no code with the C++ engine: every fill's cash, fee, position, and
average-cost realized PnL is recomputed and every reported balance compared
with what the engine published."""

from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field
from fractions import Fraction
from typing import Any


@dataclass
class Ledger:
    cash: Fraction
    position: Fraction = Fraction(0)
    fees_paid: Fraction = Fraction(0)
    gross_realized_pnl: Fraction = Fraction(0)
    open_cost_basis: Fraction = Fraction(0)
    fills: int = 0
    cash_after: list[tuple[str, Fraction]] = field(default_factory=list)

    def apply(
        self, side: str, price: Fraction, quantity: Fraction, fee: Fraction, notional: Fraction
    ) -> None:
        assert notional == price * quantity, "a fill's notional is its price times its quantity"
        self.fills += 1
        self.fees_paid += fee
        if side == "buy":
            self.cash -= notional + fee
            self.open_cost_basis += notional
            self.position += quantity
            return
        assert quantity <= self.position, "the strategy never sells more than it holds"
        cost = self.open_cost_basis * quantity / self.position
        self.gross_realized_pnl += notional - cost
        self.open_cost_basis -= cost
        self.position -= quantity
        self.cash += notional - fee


def decimal(text: str) -> Fraction:
    return Fraction(text)


def recompute(
    journal: Iterable[Mapping[str, Any]], starting_cash: str, fee_per_fill: str
) -> dict[str, Ledger]:
    """Every account's ledger after every execution in the journal."""
    ledgers: dict[str, Ledger] = {}
    orders: dict[str, Mapping[str, Any]] = {}
    for entry in journal:
        if entry["type"] == "order":
            orders[entry["client_order_id"]] = entry
        if entry["type"] != "execution" or entry["fill"] is None:
            continue
        ledger = ledgers.setdefault(entry["account"], Ledger(cash=decimal(starting_cash)))
        fill = entry["fill"]
        assert decimal(fill["fee"]) == decimal(fee_per_fill), "every fill pays fee_per_fill"
        ledger.apply(
            orders[entry["client_order_id"]]["side"],
            decimal(fill["price"]),
            decimal(fill["quantity"]),
            decimal(fill["fee"]),
            decimal(fill["notional"]),
        )
        assert decimal(entry["cash_after"]) == ledger.cash, "the journal's cash after a fill"
        assert decimal(entry["position_after"]) == ledger.position, "the journal's position after a fill"
    return ledgers


def check_run(manifest: Mapping[str, Any], journal: Iterable[Mapping[str, Any]]) -> dict[str, dict[str, str]]:
    """Recomputes every account and requires the engine's final ledger and
    valuation to agree exactly; returns the recomputed figures."""
    configuration = manifest["configuration"]
    ledgers = recompute(journal, configuration["starting_cash"], configuration["execution"]["fee_per_fill"])
    recomputed: dict[str, dict[str, str]] = {}
    for account, results in manifest["results"]["accounts"].items():
        ledger = ledgers.get(account, Ledger(cash=decimal(configuration["starting_cash"])))
        reported = results["final"]["ledger"]
        for key in ("cash", "position", "fees_paid", "gross_realized_pnl", "open_cost_basis"):
            assert decimal(reported[key]) == getattr(ledger, key), f"{account} {key}"
        assert int(results["fills"]) == ledger.fills, f"{account} fills"
        valuation = results["final"]["valuation"]
        figures = {
            "cash": str(ledger.cash),
            "position": str(ledger.position),
            "fees_paid": str(ledger.fees_paid),
        }
        if "unavailable" not in valuation:
            bid = decimal(valuation["mark"]["bid"]) if valuation["mark"] is not None else Fraction(0)
            equity = ledger.cash + ledger.position * bid
            assert decimal(valuation["equity"]) == equity, f"{account} equity"
            assert decimal(valuation["net_total_pnl"]) == equity - decimal(configuration["starting_cash"]), (
                f"{account} net PnL"
            )
            figures["equity"] = str(equity)
        recomputed[account] = figures
    return recomputed
