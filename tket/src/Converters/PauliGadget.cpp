// Copyright 2019-2023 Cambridge Quantum Computing
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tket/Converters/PauliGadget.hpp"

#include "tket/Circuit/CircUtils.hpp"
#include "tket/Circuit/ConjugationBox.hpp"
#include "tket/Circuit/PauliExpBoxes.hpp"

namespace tket {

Expr pauli_angle_convert_or_throw(Complex pauliCoeff, const Expr &angle) {
  if (pauliCoeff == -1.) {
    return -1 * angle;
  }
  if (pauliCoeff != 1.) {
    throw CircuitInvalidity("Pauli coefficient must be +/- 1");
  }
  return angle;
}

void append_single_pauli_gadget(
    Circuit &circ, const QubitPauliTensor &pauli, Expr angle,
    CXConfigType cx_config) {
  auto converted_angle = pauli_angle_convert_or_throw(pauli.coeff, angle);

  std::vector<Pauli> string;
  unit_map_t mapping;
  unsigned i = 0;
  for (const std::pair<const Qubit, Pauli> &term : pauli.string.map) {
    string.push_back(term.second);
    mapping.insert({Qubit(q_default_reg(), i), term.first});
    i++;
  }
  Circuit gadget = pauli_gadget(string, converted_angle, cx_config);
  circ.append_with_map(gadget, mapping);
}

void append_single_pauli_gadget_as_pauli_exp_box(
    Circuit &circ, const QubitPauliTensor &pauli, Expr angle,
    CXConfigType cx_config) {
  auto converted_angle = pauli_angle_convert_or_throw(pauli.coeff, angle);

  std::vector<Pauli> string;
  std::vector<Qubit> mapping;
  for (const std::pair<const Qubit, Pauli> &term : pauli.string.map) {
    string.push_back(term.second);
    mapping.push_back(term.first);
  }
  PauliExpBox box(string, converted_angle, cx_config);
  circ.add_box(box, mapping);
}

void append_pauli_gadget_pair_as_box(
    Circuit &circ, const QubitPauliTensor &pauli0, Expr angle0,
    const QubitPauliTensor &pauli1, Expr angle1, CXConfigType cx_config) {
  auto converted_angle0 = pauli_angle_convert_or_throw(pauli0.coeff, angle0);
  auto converted_angle1 = pauli_angle_convert_or_throw(pauli1.coeff, angle1);

  std::vector<Qubit> mapping;
  std::vector<Pauli> paulis0;
  std::vector<Pauli> paulis1;

  QubitPauliString pauli0_string(pauli0.string);
  QubitPauliString pauli1_string(pauli1.string);

  // remove any identities
  pauli0_string.compress();
  pauli1_string.compress();

  // add paulis for qubits in pauli0_string
  for (const std::pair<const Qubit, Pauli> &term : pauli0_string.map) {
    mapping.push_back(term.first);
    paulis0.push_back(term.second);
    auto found = pauli1_string.map.find(term.first);
    if (found == pauli1_string.map.end()) {
      paulis1.push_back(Pauli::I);
    } else {
      paulis1.push_back(found->second);
      pauli1_string.map.erase(found);
    }
  }
  // add paulis for qubits in pauli1_string that weren't in pauli0_string
  for (const std::pair<const Qubit, Pauli> &term : pauli1_string.map) {
    mapping.push_back(term.first);
    paulis1.push_back(term.second);
    paulis0.push_back(Pauli::I);  // If pauli0_string contained qubit, would
                                  // have been handled above
  }
  PauliExpPairBox box(
      paulis0, converted_angle0, paulis1, converted_angle1, cx_config);
  circ.add_box(box, mapping);
}

void append_commuting_pauli_gadget_set_as_box(
    Circuit &circ, const std::list<std::pair<QubitPauliTensor, Expr>> &gadgets,
    CXConfigType cx_config) {
  // Translate to QubitPauliTensors to vectors of Paulis of same length
  // Preserves ordering of qubits

  std::set<Qubit> all_qubits;
  for (const auto &gadget : gadgets) {
    for (const auto &qubit_pauli : gadget.first.string.map) {
      all_qubits.insert(qubit_pauli.first);
    }
  }

  std::vector<Qubit> mapping;
  for (const auto &qubit : all_qubits) {
    mapping.push_back(qubit);
  }

  std::vector<std::pair<std::vector<Pauli>, Expr>> pauli_gadgets;
  for (const auto &gadget : gadgets) {
    auto &new_gadget = pauli_gadgets.emplace_back(std::make_pair(
        std::vector<Pauli>(),
        pauli_angle_convert_or_throw(gadget.first.coeff, gadget.second)));
    for (const auto &qubit : mapping) {
      auto found = gadget.first.string.map.find(qubit);
      if (found == gadget.first.string.map.end()) {
        new_gadget.first.push_back(Pauli::I);
      } else {
        new_gadget.first.push_back(found->second);
      }
    }
  }

  PauliExpCommutingSetBox box(pauli_gadgets, cx_config);
  circ.add_box(box, mapping);
}

static void reduce_shared_qs_by_CX_snake(
    Circuit &circ, std::set<Qubit> &match, QubitPauliTensor &pauli0,
    QubitPauliTensor &pauli1) {
  unsigned match_size = match.size();
  while (match_size > 1) {  // We allow one match left over
    auto it = --match.end();
    Qubit to_eliminate = *it;
    match.erase(it);
    Qubit helper = *match.rbegin();
    // extend CX snake
    circ.add_op<Qubit>(OpType::CX, {to_eliminate, helper});
    pauli0.string.map.erase(to_eliminate);
    pauli1.string.map.erase(to_eliminate);
    match_size--;
  }
}

static void reduce_shared_qs_by_CX_star(
    Circuit &circ, std::set<Qubit> &match, QubitPauliTensor &pauli0,
    QubitPauliTensor &pauli1) {
  std::set<Qubit>::iterator iter = match.begin();
  for (std::set<Qubit>::iterator next = match.begin(); match.size() > 1;
       iter = next) {
    ++next;
    Qubit to_eliminate = *iter;
    circ.add_op<Qubit>(OpType::CX, {to_eliminate, *match.rbegin()});
    pauli0.string.map.erase(to_eliminate);
    pauli1.string.map.erase(to_eliminate);
    match.erase(iter);
  }
}

static void reduce_shared_qs_by_CX_tree(
    Circuit &circ, std::set<Qubit> &match, QubitPauliTensor &pauli0,
    QubitPauliTensor &pauli1) {
  while (match.size() > 1) {
    std::set<Qubit> remaining;
    std::set<Qubit>::iterator it = match.begin();
    while (it != match.end()) {
      Qubit maintained = *it;
      it++;
      remaining.insert(maintained);
      if (it != match.end()) {
        Qubit to_eliminate = *it;
        it++;
        circ.add_op<Qubit>(OpType::CX, {to_eliminate, maintained});
        pauli0.string.map.erase(to_eliminate);
        pauli1.string.map.erase(to_eliminate);
      }
    }
    match = remaining;
  }
}

static void reduce_shared_qs_by_CX_multiqgate(
    Circuit &circ, std::set<Qubit> &match, QubitPauliTensor &pauli0,
    QubitPauliTensor &pauli1) {
  if (match.size() <= 1) {
    return;
  }
  // last qubit is target
  Qubit target = *match.rbegin();
  while (match.size() > 1) {
    std::set<Qubit>::iterator iter = match.begin();
    if (match.size() == 2) {
      // use CX
      Qubit to_eliminate = *iter;
      match.erase(iter);
      pauli0.string.map.erase(to_eliminate);
      pauli1.string.map.erase(to_eliminate);

      circ.add_op<Qubit>(OpType::CX, {to_eliminate, target});
    } else {
      // use XXPhase3
      Qubit to_eliminate1 = *iter;
      match.erase(iter++);
      pauli0.string.map.erase(to_eliminate1);
      pauli1.string.map.erase(to_eliminate1);

      Qubit to_eliminate2 = *iter;
      match.erase(iter);
      pauli0.string.map.erase(to_eliminate2);
      pauli1.string.map.erase(to_eliminate2);

      circ.add_op<Qubit>(OpType::H, {to_eliminate1});
      circ.add_op<Qubit>(OpType::H, {to_eliminate2});
      circ.add_op<Qubit>(
          OpType::XXPhase3, 0.5, {to_eliminate1, to_eliminate2, target});
      circ.add_op<Qubit>(OpType::X, {target});
    }
  }
}

void append_pauli_gadget_pair(
    Circuit &circ, QubitPauliTensor pauli0, Expr angle0,
    QubitPauliTensor pauli1, Expr angle1, CXConfigType cx_config) {
  /*
   * Cowtan, Dilkes, Duncan, Simmons, Sivarajah: Phase Gadget Synthesis for
   * Shallow Circuits, Lemma 4.9
   * Let s and t be Pauli strings; then there exists a Clifford unitary U such
   * that
   * P(a, s) . P(b, t) = U . P(a, s') . P(b, t') . U^\dagger
   * where s' and t' are Pauli strings with intersection at most 1.
   *
   * Follows the procedure to reduce the intersection of the gadgets and then
   * synthesises the remainder individually.
   */
  pauli0.compress();
  pauli1.compress();

  /*
   * Step 1: Partition qubits into those just affected by pauli0 (just0) and
   * pauli1 (just1), and those in both which either match or don't
   */
  std::set<Qubit> just0 = pauli0.own_qubits(pauli1);
  std::set<Qubit> just1 = pauli1.own_qubits(pauli0);
  std::set<Qubit> match = pauli0.common_qubits(pauli1);
  std::set<Qubit> mismatch = pauli0.conflicting_qubits(pauli1);

  /*
   * Step 2: Build the unitary U that minimises the intersection of the gadgets.
   */
  Circuit u;
  for (const Qubit &qb : just0) u.add_qubit(qb);
  for (const Qubit &qb : just1) u.add_qubit(qb);
  for (const Qubit &qb : match) u.add_qubit(qb);
  for (const Qubit &qb : mismatch) u.add_qubit(qb);
  Circuit v(u);

  /*
   * Step 2.i: Remove (almost) all matches by converting to Z basis and applying
   * CXs
   */
  for (const Qubit &qb : match) {
    switch (pauli0.string.map.at(qb)) {
      case Pauli::X:
        u.add_op<Qubit>(OpType::H, {qb});
        pauli0.string.map.at(qb) = Pauli::Z;
        pauli1.string.map.at(qb) = Pauli::Z;
        break;
      case Pauli::Y:
        u.add_op<Qubit>(OpType::V, {qb});
        pauli0.string.map.at(qb) = Pauli::Z;
        pauli1.string.map.at(qb) = Pauli::Z;
        break;
      default:
        break;
    }
  }
  switch (cx_config) {
    case CXConfigType::Snake: {
      reduce_shared_qs_by_CX_snake(u, match, pauli0, pauli1);
      break;
    }
    case CXConfigType::Star: {
      reduce_shared_qs_by_CX_star(u, match, pauli0, pauli1);
      break;
    }
    case CXConfigType::Tree: {
      reduce_shared_qs_by_CX_tree(u, match, pauli0, pauli1);
      break;
    }
    case CXConfigType::MultiQGate: {
      reduce_shared_qs_by_CX_multiqgate(u, match, pauli0, pauli1);
      break;
    }
    default:
      throw UnknownCXConfigType();
  }
  /*
   * Step 2.ii: Convert mismatches to Z in pauli0 and X in pauli1
   */
  for (const Qubit &qb : mismatch) {
    switch (pauli0.string.map.at(qb)) {
      case Pauli::X: {
        switch (pauli1.string.map.at(qb)) {
          case Pauli::Y:
            u.add_op<Qubit>(OpType::Sdg, {qb});
            u.add_op<Qubit>(OpType::Vdg, {qb});
            break;
          case Pauli::Z:
            u.add_op<Qubit>(OpType::H, {qb});
            break;
          default:
            break;  // Cannot hit this case
        }
        break;
      }
      case Pauli::Y: {
        switch (pauli1.string.map.at(qb)) {
          case Pauli::X:
            u.add_op<Qubit>(OpType::V, {qb});
            break;
          case Pauli::Z:
            u.add_op<Qubit>(OpType::V, {qb});
            u.add_op<Qubit>(OpType::S, {qb});
            break;
          default:
            break;  // Cannot hit this case
        }
        break;
      }
      default: {  // Necessarily Z
        if (pauli1.string.map.at(qb) == Pauli::Y)
          u.add_op<Qubit>(OpType::Sdg, {qb});
        // No need to act if already X
      }
    }
    pauli0.string.map.at(qb) = Pauli::Z;
    pauli1.string.map.at(qb) = Pauli::X;
  }

  /*
   * Step 2.iii: Remove the final matching qubit against a mismatch if one
   * exists, otherwise allow both gadgets to build it
   */
  if (!match.empty()) {
    Qubit last_match = *match.begin();
    match.erase(last_match);
    if (!mismatch.empty()) {
      Qubit mismatch_used =
          *mismatch.rbegin();  // Prefer to use the one that may be left over
                               // after reducing pairs
      u.add_op<Qubit>(OpType::S, {mismatch_used});
      u.add_op<Qubit>(OpType::CX, {last_match, mismatch_used});
      u.add_op<Qubit>(OpType::Sdg, {mismatch_used});
      pauli0.string.map.erase(last_match);
      pauli1.string.map.erase(last_match);
    } else {
      just0.insert(last_match);
      just1.insert(last_match);
    }
  }

  /*
   * Step 2.iv: Reduce pairs of mismatches to different qubits.
   * Allow both gadgets to build a remaining qubit if it exists.
   */
  std::set<Qubit>::iterator mis_it = mismatch.begin();
  while (mis_it != mismatch.end()) {
    Qubit z_in_0 = *mis_it;
    just0.insert(z_in_0);
    mis_it++;
    if (mis_it == mismatch.end()) {
      just1.insert(z_in_0);
    } else {
      Qubit x_in_1 = *mis_it;
      u.add_op<Qubit>(OpType::CX, {x_in_1, z_in_0});
      pauli0.string.map.erase(x_in_1);
      pauli1.string.map.erase(z_in_0);
      just1.insert(x_in_1);
      mis_it++;
    }
  }

  /*
   * Step 3: Combine circuits to give final result
   */
  append_single_pauli_gadget(v, pauli0, angle0);
  append_single_pauli_gadget(v, pauli1, angle1);
  ConjugationBox cjbox(
      std::make_shared<CircBox>(u), std::make_shared<CircBox>(v));
  circ.add_box(cjbox, u.all_qubits());
}

}  // namespace tket
