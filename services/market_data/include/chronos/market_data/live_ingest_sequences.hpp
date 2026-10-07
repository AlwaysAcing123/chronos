#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace chronos::market_data {

// Why a live source may not number or publish events now (ADR 0020).
struct IngestSequencesUnavailable {
    // Short and free of credentials, for the source's status and log.
    std::string diagnostic;
    // Waiting cannot help: the stream's sequences are exhausted, or its
    // durable history needs an operator's decision. The source fails
    // instead of retrying.
    bool permanent{false};
};

// Whether a source may still publish an event it has numbered. It is asked
// under the market-event gate, immediately before the event is applied.
// This is an in-process eligibility check, not a distributed fence: a pause
// between this check and apply can still allow a local update. The database
// fence prevents a deposed generation's authoritative effects (ADR 0020).
class PublicationPermit {
public:
    virtual ~PublicationPermit() = default;

    // Why an event numbered in publication run `run` may not be published
    // now, or nullopt while it may. Once a run has ended this never returns
    // nullopt for it again, whatever happens to ownership later.
    [[nodiscard]] virtual std::optional<IngestSequencesUnavailable>
    publication_refusal(std::uint64_t run) const = 0;
};

using IssuedSequence = std::variant<std::uint64_t, IngestSequencesUnavailable>;

// One publication run: the sequences issued from one begin_run() until the
// run ends. Run IDs only increase and are never reused, so an event numbered
// in one run can never be published in a later one (ADR 0020).
struct PublicationRun {
    std::uint64_t id{0};
    // The run's first sequence.
    std::uint64_t first{0};
};

using BegunRun = std::variant<PublicationRun, IngestSequencesUnavailable>;

// The ingest sequences of one live stream, issued to its one source.
//
// Sequences only increase and are never issued twice, by this process or by
// any later one that continues the stream. Within one publication run they
// are contiguous. A new run may begin later than the last sequence issued,
// never earlier; the source declares that jump to the market state while it
// is suspended (MarketState::continue_at), so no quote is executable across
// it. Only the source's own thread calls begin_run() and issue().
//
// A run ends when the stream stops being this process's to write: its
// ownership is lost or lapses, even briefly, or is acquired again. It also
// ends when it reaches a reserved block that does not continue it, or when
// another run begins. An ended run issues and publishes nothing more; the
// source begins a new one only on a new upstream session.
class LiveIngestSequences : public PublicationPermit {
public:
    // Why a run could not begin now, or nullopt. Changes nothing, so a
    // source can ask before opening an upstream connection.
    [[nodiscard]] virtual std::optional<IngestSequencesUnavailable> readiness() const = 0;

    // Ends any current run and begins a new one.
    [[nodiscard]] virtual BegunRun begin_run() = 0;

    // Issues `count` (at least one) consecutive sequences of run `run`, for
    // events published together, and returns the first. Either all are
    // issued or none is; none is once the run has ended.
    [[nodiscard]] virtual IssuedSequence issue(std::uint64_t run, std::uint64_t count) = 0;
};

}  // namespace chronos::market_data
