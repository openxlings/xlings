module xlings.cli.completion;

import std;
import xlings.cli.spec;

namespace xlings::cli::completion {

std::vector<Candidate> complete(std::span<const std::string> words, const Provider& provider) {
    return complete_in(spec::root(), words, provider);
}

}  // namespace xlings::cli::completion
