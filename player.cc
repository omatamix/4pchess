#include <tuple>
#include <algorithm>
#include <cmath>
#include <utility>
#include <cassert>
#include <functional>
#include <optional>
#include <iostream>
#include <tuple>
#include <stdexcept>
#include <cstring>
#include <thread>
#include <mutex>

#include "board.h"
#include "player.h"
#include "move_picker.h"
//#include "static_exchange.h"

namespace chess {

AlphaBetaPlayer::AlphaBetaPlayer(std::optional<PlayerOptions> options) {
  if (options.has_value()) {
    options_ = *options;
  }

  piece_move_order_scores_[PAWN] = 1;
  piece_move_order_scores_[KNIGHT] = 2;
  piece_move_order_scores_[BISHOP] = 3;
  piece_move_order_scores_[ROOK] = 4;
  piece_move_order_scores_[QUEEN] = 5;
  piece_move_order_scores_[KING] = 0;

  king_attacker_values_[PAWN] = 25;
  king_attacker_values_[KNIGHT] = 30;
  king_attacker_values_[BISHOP] = 30;
  king_attacker_values_[ROOK] = 40;
  king_attacker_values_[QUEEN] = 50;
  king_attacker_values_[KING] = 0;

  if (options_.enable_transposition_table) {
    transposition_table_ = std::make_unique<TranspositionTable>(
        options_.transposition_table_size);
  }

  for (int row = 0; row < 14; row++) {
    for (int col = 0; col < 14; col++) {
      if (row <= 2 || row >= 11 || col <= 2 || col >= 11) {
        location_evaluations_[row][col] = 5;
      } else if (row <= 4 || row >= 9 || col <= 4 || col >= 9) {
        location_evaluations_[row][col] = 10;
      } else {
        location_evaluations_[row][col] = 15;
      }
    }
  }

  king_attack_weight_[0] = 0;
  king_attack_weight_[1] = 50;
  king_attack_weight_[2] = 100;
  king_attack_weight_[3] = 120;
  king_attack_weight_[4] = 150;
  king_attack_weight_[5] = 200;
  king_attack_weight_[6] = 250;
  king_attack_weight_[7] = 300;
  for (int i = 8; i < 30; i++) {
    king_attack_weight_[i] = 400;
  }

  if (options_.enable_piece_square_table) {
    for (int cl = 0; cl < 4; cl++) {
      PlayerColor color = static_cast<PlayerColor>(cl);
      for (int pt = 0; pt < 6; pt++) {
        PieceType piece_type = static_cast<PieceType>(pt);
        bool is_piece = (piece_type == QUEEN || piece_type == ROOK
                         || piece_type == BISHOP || piece_type == KNIGHT);

        for (int row = 0; row < 14; row++) {
          for (int col = 0; col < 14; col++) {
            int table_value = 0;

            if (is_piece) {
              // preference for centrality
              float center_dist = std::sqrt((row - 6.5) * (row - 6.5)
                                          + (col - 6.5) * (col - 6.5));
              table_value -= (int)(10 * center_dist);

              // preference for pieces on opponent team's back-3 rank
              if (color == RED || color == YELLOW) {
                if (col < 3 || col >= 11) {
                  table_value += 10;
                }
              } else {
                if (row < 3 || row >= 11) {
                  table_value += 10;
                }
              }
            }

            piece_square_table_[color][piece_type][row][col] = table_value;
          }
        }
      }
    }
  }

  if (options_.enable_piece_activation) {
    piece_activation_threshold_[KING] = 999;
    piece_activation_threshold_[PAWN] = 999;
    piece_activation_threshold_[NO_PIECE] = 999;
    piece_activation_threshold_[QUEEN] = 5;
    piece_activation_threshold_[BISHOP] = 5;
    piece_activation_threshold_[KNIGHT] = 3;
    piece_activation_threshold_[ROOK] = 5;
  }

  if (options_.enable_knight_bonus) {
    std::memset(knight_to_king_, 0, 14*14*14*14 * sizeof(bool) / sizeof(char));
    for (int row = 0; row < 14; ++row) {
      for (int col = 0; col < 14; ++col) {
        // first move
        for (int dr : {-2, -1, 1, 2}) {
          int r1 = row + dr;
          if (r1 < 0 || r1 > 13) {
            continue;
          }
          int abs_dc = std::abs(dr) == 1 ? 2 : 1;
          for (int dc : {-abs_dc, abs_dc}) {
            int c1 = col + dc;
            if (c1 < 0 || c1 > 13) {
              continue;
            }

            // second move
            for (int dr2 : {-2, -1, 1, 2}) {
              int r2 = r1 + dr2;
              if (r2 < 0 || r2 > 13) {
                continue;
              }
              int abs_dc2 = std::abs(dr2) == 1 ? 2 : 1;
              for (int dc2 : {-abs_dc2, abs_dc2}) {
                int c2 = c1 + dc2;
                if (c2 < 0 || c2 > 13) {
                  continue;
                }
                knight_to_king_[row][col][r2][c2] = true;
              }
            }
          }
        }
      }
    }
  }
}


ThreadState::ThreadState(
    PlayerOptions options, const Board& board, const PVInfo& pv_info)
  : options_(options), board_(board), pv_info_(pv_info) {
  move_buffer_ = new Move[kBufferPartitionSize * kBufferNumPartitions];
  counter_moves = new Move[14*14*14*14];
  continuation_history = new ContinuationHistory*[2];
  for (int i = 0; i < 2; i++) {
    continuation_history[i] = new ContinuationHistory[2];
  }
}

ThreadState::~ThreadState() {
  delete[] move_buffer_;
  delete[] counter_moves;
  for (int i = 0; i < 2; i++) {
    delete[] continuation_history[i];
  }
  delete[] continuation_history;
}

Move* ThreadState::GetNextMoveBufferPartition() {
  if (buffer_id_ >= kBufferNumPartitions) {
    std::cout << "ThreadState move buffer overflow" << std::endl;
    abort();
  }
  return &move_buffer_[buffer_id_++ * kBufferPartitionSize];
}

void ThreadState::ReleaseMoveBufferPartition() {
  assert(buffer_id_ > 0);
  buffer_id_--;
}

int AlphaBetaPlayer::GetNumLegalMoves(Board& board) {
  constexpr int kLimit = 300;
  Move moves[kLimit];
  Player player = board.GetTurn();
  size_t num_moves = board.GetPseudoLegalMoves2(moves, kLimit);
  int n_legal = 0;
  for (size_t i = 0; i < num_moves; i++) {
    const auto& move = moves[i];
    board.MakeMove(move);
    if (!board.IsKingInCheck(player)) { // invalid move
      n_legal++;
    }
    board.UndoMove();
  }

  return n_legal;
}

// Alpha-beta search with nega-max framework.
// https://www.chessprogramming.org/Alpha-Beta
// Returns (nega-max value, best move) pair.
// The best move is nullopt if the game is over.
// If the function returns std::nullopt, then it hit the deadline
// before finishing search and the results should not be used.
std::optional<std::tuple<int, std::optional<Move>>> AlphaBetaPlayer::Search(
    Stack* ss,
    NodeType node_type,
    ThreadState& thread_state,
    int ply,
    int depth,
    int alpha,
    int beta,
    bool maximizing_player,
    int expanded,
    const std::optional<
        std::chrono::time_point<std::chrono::system_clock>>& deadline,
    PVInfo& pvinfo,
    int null_moves,
    bool is_cut_node) {
  Board& board = thread_state.GetBoard();
  depth = std::max(depth, 0);
  if (canceled_
      || (deadline.has_value()
        && std::chrono::system_clock::now() >= *deadline)) {
    return std::nullopt;
  }

  // increase node count
  num_nodes_++;

  // root node detection
  const bool is_root_node = ply == 1;

  // pv node detection
  const bool is_pv_node = node_type != NonPV;

  // all node detection
  const bool allNode = !(is_pv_node || is_cut_node);

  // get next players turn
  Player player = board.GetTurn();

  // check for depth termination
  if (depth <= 0) {
    if (options_.enable_qsearch) {
      return QSearch(ss, is_pv_node ? PV : NonPV, thread_state, 0, alpha, beta, maximizing_player, deadline, pvinfo);
    }

    // if qsearch is disabled just return the eval
    int eval = Evaluate(thread_state, maximizing_player, alpha, beta);
    
    // if tt is enabled then save the eval for future searches
    if (options_.enable_transposition_table) {
      transposition_table_->Save(board.HashKey(), 0, std::nullopt, 0, eval, EXACT, is_pv_node);
    }

    // return the eval
    return std::make_tuple(eval, std::nullopt);
  }

  // define the tt node status for former pv nodes
  // define the tt hit status from the tt data entry
  bool is_tt_pv = false, tt_hit = false;

  std::optional<Move> tt_move;
  const HashTableEntry* tte = nullptr;

  if (options_.enable_transposition_table)
  {
    int64_t key = board.HashKey();

    tte = transposition_table_->Get(key);
    if (tte != nullptr)
    {
      if (tte->key == key)
      {
        // valid entry
        if (tte->depth >= depth)
        {
          num_cache_hits_++;

          // at non pv nodes check for an early TT cutoff
          if (!is_root_node && !is_pv_node && (tte->bound == EXACT
             || (tte->bound == LOWER_BOUND && tte->score >= beta)
             || (tte->bound == UPPER_BOUND && tte->score <= alpha))
          ) {
            return std::make_tuple(std::min(beta, std::max(alpha, tte->score)), tte->move);
          }
        }
       
        // update tt vars
        tt_hit   = true;
        tt_move  = tte->move;
        is_tt_pv = tte->is_pv;
      }
    }
  }
  
  // get the prior reduction
  int prior_reduction = (ss - 1)->reduction;
  
  // reset the prior reduction
  (ss - 1)->reduction = 0;

  // check to see if the cuurent player to move is in check
  bool in_check = board.IsKingInCheck(player);

  // check to see if the partner of the current player to move is in check
  bool partner_checked = board.IsKingInCheck(GetPartner(player));

  // overall check to see if the team is in check
  bool team_checked = in_check || partner_checked;

  // save check info to the stack table
  ss->in_check = team_checked;

  // eval improvement vars
  bool improving = false, declining = false;

  // other base vals
  int eval = value_none_tt, do_move_level_pruning = true;

  if (ss->in_check) {
    // skip early pruning
    ss->static_eval = eval = (ss - 2)->static_eval;
    // improving              = false;
    do_move_level_pruning  = false;
  }
  else if (tt_hit) 
  {
    if (tte->eval == value_none_tt)
    {
      ss->static_eval = eval = Evaluate(thread_state, maximizing_player, alpha, beta);
    }
    else
    {
      ss->static_eval = eval = tte->eval;
    }
  }
  else
  {
    ss->static_eval = eval = Evaluate(thread_state, maximizing_player, alpha, beta);
    transposition_table_->Save(board.HashKey(), depth, std::nullopt, 0, eval, EXACT, is_pv_node);
  }
  
  // reset killers
  (ss + 2)->killers[0] = (ss + 2)->killers[1] = Move();
  
  // reset move count
  ss->move_count = 0;

  // update root depth info
  if (ply == 1)
  {
    ss->root_depth = depth;
  }

  (ss + 1)->root_depth = ss->root_depth;

  // move level pruning
  if (do_move_level_pruning)
  {
    // this is per stockfishes way
    improving = ply > 2 &&  (ss - 2)->static_eval != value_none_tt
                        &&  (ss - 2)->static_eval < ss->static_eval;
    declining = ply > 1 && -(ss - 1)->static_eval < ss->static_eval
                        &&  (ss - 1)->static_eval != value_none_tt;

    if (!is_pv_node)
    {

      // reverse futility pruning
      if (options_.enable_futility_pruning
        && !is_tt_pv && depth <= 2 - improving
        && eval - 150 * depth >= beta
        && eval < kMateValue
      ) {
        return std::make_tuple(beta, std::nullopt);
      }

      // null move pruning
      if (options_.enable_null_move_pruning
        && !is_root_node     // not root
        && null_moves == 0   // last move wasn't null
        && eval >= beta + 50 // check against beta adjustment
      ) {
        num_null_moves_tried_++;
        ss->continuation_history = &thread_state.continuation_history[0][0][NO_PIECE][0][0];
        ss->current_move = Move();
        board.MakeNullMove();

        // try the null move with possibly reduced depth
        PVInfo null_pvinfo;
        int r = std::min(depth / 3 + 2, depth);

        auto value_and_move_or = Search(
          ss+1, NonPV, thread_state, ply + 1, depth - r,
          -beta, -beta + 1, !maximizing_player, expanded, deadline, null_pvinfo,
          null_moves + 1
        );

        board.UndoNullMove();

        // if it failed high, skip this move
        if (value_and_move_or.has_value())
        {
          int nmp_score = -std::get<0>(*value_and_move_or);
          
          // null move verification
          if (depth >= 256)
          // {
          //   auto value_and_move_or_nmv = Search(
          //     ss + 1, NonPV, thread_state, ply + 1, depth - r,
          //     alpha, beta, maximizing_player, expanded, deadline, null_pvinfo, null_moves + 1
          // );

          // if (value_and_move_or_nmv.has_value()) {
          //   int verify_score = std::get<0>(*value_and_move_or_nmv);
            
          //   if (verify_score >= beta)
          //   {
          //     num_null_moves_pruned_++;

          //     return std::make_tuple(verify_score, std::nullopt);
          //   }
          // }
          }
          else
          {
            if (nmp_score >= beta
              // don't return unproven mate score
              && nmp_score < kMateValue
            ) {
              num_null_moves_pruned_++;

              return std::make_tuple(beta, std::nullopt);
            }
          }
        }
      }
    }

    // IID
    if (depth >= 9 && !tt_move) depth -= 1 + is_cut_node;
  }

  std::optional<Move> best_move;
  int player_color = static_cast<int>(player.GetColor());

  int curr_n_activated = thread_state.NActivated()[player_color];
  int curr_total_moves = thread_state.TotalMoves()[player_color];

  const PieceToHistory* cont_hist[] = {
    (ss - 1)->continuation_history,
    (ss - 2)->continuation_history,
    (ss - 3)->continuation_history,
    (ss - 4)->continuation_history,
    (ss - 5)->continuation_history,
  };

  std::optional<Move> pv_move = pvinfo.GetBestMove();
  Move* moves = thread_state.GetNextMoveBufferPartition();
  MovePicker move_picker(
    board,
    pv_move.has_value() ? pv_move : tt_move,
    ss->killers,
    kPieceEvaluations,
    thread_state.history_heuristic,
    thread_state.capture_heuristic,
    piece_move_order_scores_,
    options_.enable_move_order_checks,
    moves,
    kBufferPartitionSize
   , thread_state.counter_moves
   , /*include_quiets=*/true
   , cont_hist
    );

  bool has_legal_moves = false;
  int move_count = 0;
  int quiets = 0;
  bool fail_low = true;
  bool fail_high = false;
  std::vector<Move> searched_moves;

  while (true) {
    Move* move_ptr = move_picker.GetNextMove();
    if (move_ptr == nullptr) {
      break;
    }

    Move& move = *move_ptr;
    const auto& from = move.From();
    const auto& to = move.To();
    Piece piece = board.GetPiece(move.From());
    PieceType piece_type = piece.GetPieceType();

    std::optional<std::tuple<int, std::optional<Move>>> value_and_move_or;

    // this has to be called before the move is made
    bool delivers_check = move.DeliversCheck(board);

    bool lmr =
      options_.enable_late_move_reduction
      && depth > 1
      && move_count > 1 + is_root_node + is_pv_node
      && (!is_tt_pv
          || !move.IsCapture()
          || (is_cut_node && (ss-1)->move_count > 1))
         ;

    bool quiet = !in_check && !move.IsCapture() && !delivers_check
      ;

    // late move pruning threshold
    int q = 1 + depth*depth/(declining?10:5);
    if (is_pv_node) {
      q = 5 + depth*depth/(declining?2:1);
      if (improving) {
        q *= 2;
      }
    }

    if (options_.enable_late_move_pruning
        && alpha > -kMateValue  // don't prune if we're mated
        && quiet
        && quiets >= q
        ) {
      num_lm_pruned_++;
      continue;
    }

    // is this a killer move
    const int is_killer = ss->killers[0] == move || ss->killers[1] == move;

    int r = 1 + std::max(0,(depth-5)/3) + move_count/30;

    // increase reduction more if the move is a quiet move
    if (quiet) 
    {
      r++;
      
      // gradually increase reduction the higher in depth we go
      r += depth / 8;
    }

    // decrease reduction for killer moves
    r -= is_killer;

    // increase reduction if the static eval is too far from alpha
    r += std::min(2, std::abs(eval - alpha) / 350);

    // decrease reduction if the node is a tt pv node
    r -= is_tt_pv;

    // increase reduction if this node is cut
    if (is_cut_node) r += 2;

    // decrease reduction if the opponent is worsening
    // increase reduction if we are not improving
    r -= declining - !improving;

    // decrease reduction if we are in check
    r -= in_check;

    // decrease reduction if the move delivers check
    r -= delivers_check;

    // decrease reduction if the node is a pv node
    r -= is_pv_node;

    // decrease reduction if the move is a capture mode and has a positive SEE val
    r -= move.IsCapture() && move.ApproxSEE(board, kPieceEvaluations) > 0;

    if (!move.IsCapture()) {
      int history_score = thread_state.history_heuristic[piece.GetPieceType()][from.GetRow()][from.GetCol()]
          [to.GetRow()][to.GetCol()];
      r -= std::clamp((history_score - 4000) / 10000, -3, 3);
    } else {
      Piece captured = move.GetCapturePiece();
      int history_score = thread_state.capture_heuristic[piece.GetPieceType()][piece.GetColor()]
        [captured.GetPieceType()][captured.GetColor()]
        [to.GetRow()][to.GetCol()];
      r -= std::clamp((history_score - 4000) / 10000, -3, 3);
    }

    // allow limited extension if the reduction is negative
    r = std::max(ply >= ss->root_depth * 1.0 ? 0 : -1, r);

    int new_depth = depth - 1;
    int lmr_depth = new_depth;
    if (lmr) {
      lmr_depth = std::max(new_depth - r, 0);
    }

    // futility pruning
    // TODO: investigate whether "deep" futility pruning should be allowed.
    if (!is_root_node
        && !is_pv_node
        && alpha > -kMateValue
        && lmr
        && move.IsCapture()
        && lmr_depth < 10
        && !in_check) {
      Piece capture_piece = move.GetCapturePiece();
      PieceType capture_piece_type = capture_piece.GetPieceType();
      int futility_eval = eval + 400 + 291 * lmr_depth + kPieceEvaluations[capture_piece_type];
      if (futility_eval < alpha) {
        continue;
      }
    }

    ss->current_move = move;
    ss->continuation_history = &thread_state.continuation_history[ss->in_check][move.IsCapture()][piece_type][move.To().GetRow()][move.To().GetCol()];

    board.MakeMove(move);

    if (board.CheckWasLastMoveKingCapture() != IN_PROGRESS) {
      board.UndoMove();

      alpha = beta; // fail hard
      //value = kMateValue;
      best_move = move;
      pvinfo.SetBestMove(move);
      break;
    }

    if (board.IsKingInCheck(player)) { // invalid move
      board.UndoMove();

      continue;
    }

    has_legal_moves = true;

    ss->move_count = move_count++;
    if (quiet) {
      quiets++;
    }

    if (options_.enable_mobility_evaluation
        || options_.enable_piece_activation) {
      UpdateMobilityEvaluation(thread_state, player);
    }

    bool is_pv_move = pv_move.has_value() && *pv_move == move;

    std::shared_ptr<PVInfo> child_pvinfo;
    if (is_pv_move && pvinfo.GetChild() != nullptr) {
      child_pvinfo = pvinfo.GetChild();
    } else {
      child_pvinfo = std::make_shared<PVInfo>();
    }

    int e = 0;  // extension

    // check extensions
    if (options_.enable_check_extensions
      && (in_check || (delivers_check && move_count < 6 && expanded < 4))) {
      num_check_extensions_++;
      e = 1;
    }

    if (lmr) {
      num_lmr_searches_++;

      r = std::clamp(r, 0, depth - 1);

      ss->reduction = depth - 1 + e;

      value_and_move_or = Search(
        ss+1, NonPV, thread_state, ply + 1, depth - 1 - r + e,
        -alpha-1, -alpha, !maximizing_player, expanded + e,
        deadline, *child_pvinfo, /*null_moves=*/0, true);
      
      ss->reduction = 0;

      value_and_move_or = Search(
          ss+1, NonPV, thread_state, ply + 1, depth - 1 - r + e,
          -alpha-1, -alpha, !maximizing_player, expanded + e,
          deadline, *child_pvinfo, /*null_moves=*/0, true);
      if (value_and_move_or.has_value() && r > 0) {
        int score = -std::get<0>(*value_and_move_or);
        if (score > alpha) {  // re-search
          num_lmr_researches_++;
          value_and_move_or = Search(
              ss+1, NonPV, thread_state, ply + 1, depth - 1 + e,
              -alpha-1, -alpha, !maximizing_player, expanded + e,
              deadline, *child_pvinfo, /*null_moves=*/0, !is_cut_node);
        }
      }

    } else if (!is_pv_node || move_count > 1) {

      if (!tt_move.has_value()) {
        r += 2;
      }

      value_and_move_or = Search(
          ss+1, NonPV, thread_state, ply + 1, depth - 1 + e - (r > 3),
          -alpha-1, -alpha, !maximizing_player, expanded + e,
          deadline, *child_pvinfo, /*null_moves=*/0, !is_cut_node);
    }

    // For PV nodes only, do a full PV search on the first move or after a fail
    // high (in the latter case search only if value < beta), otherwise let the
    // parent node fail low with value <= alpha and try another move.
    bool full_search =
      is_pv_node
      && (move_count == 1
          || (value_and_move_or.has_value()
              && -std::get<0>(*value_and_move_or) > alpha
              && (is_root_node
                  || -std::get<0>(*value_and_move_or) < beta)
              ));

    if (full_search) {
      value_and_move_or = Search(
          ss+1, PV, thread_state, ply + 1, depth - 1 + e,
          -beta, -alpha, !maximizing_player, expanded + e,
          deadline, *child_pvinfo, /*null_moves=*/0, false);
    }

    board.UndoMove();

    if (options_.enable_mobility_evaluation
        || options_.enable_piece_activation) { // reset
      thread_state.NActivated()[player_color] = curr_n_activated;
      thread_state.TotalMoves()[player_color] = curr_total_moves;
    }

    if (!value_and_move_or.has_value()) {
      thread_state.ReleaseMoveBufferPartition();
      return std::nullopt; // timeout
    }
    int score = -std::get<0>(*value_and_move_or);
    searched_moves.push_back(move);

    if (score >= beta) {
      alpha = beta;
      best_move = move;
      pvinfo.SetChild(child_pvinfo);
      pvinfo.SetBestMove(move);
      fail_low = false;
      fail_high = true;

      break; // cutoff
    }
    if (score > alpha) {
      fail_low = false;
      alpha = score;
      best_move = move;
      pvinfo.SetChild(child_pvinfo);
      pvinfo.SetBestMove(move);
    }

    if (!best_move.has_value()) {
      best_move = move;
      pvinfo.SetChild(child_pvinfo);
      pvinfo.SetBestMove(move);
    }
  }

  if (!fail_low) {
    UpdateStats(ss, thread_state, board, *best_move, depth, fail_high,
                searched_moves);
  }

  int score = alpha;
  if (!has_legal_moves) {
    if (!in_check) {
      // stalemate
      score = std::min(beta, std::max(alpha, 0));
    } else {
      // checkmate
      score = std::min(beta, std::max(alpha, -kMateValue));
    }
  }

  if (options_.enable_transposition_table) {
    ScoreBound bound = beta <= alpha ? LOWER_BOUND : is_pv_node &&
      best_move.has_value() ? EXACT : UPPER_BOUND;
    transposition_table_->Save(board.HashKey(), depth, best_move, score, eval, bound, is_pv_node);
  }

  if (best_move.has_value()
      && !best_move->IsCapture()) {
    UpdateQuietStats(ss, *best_move);
  }

  // If no good move is found and the previous position was tt_pv, then the
  // previous opponent move is probably good and the new position is added to
  // the search tree.
  if (score <= alpha) {
    ss->tt_pv = ss->tt_pv || ((ss-1)->tt_pv && depth > 3);
  }

  thread_state.ReleaseMoveBufferPartition();
  return std::make_tuple(score, best_move);
}

std::optional<std::tuple<int, std::optional<Move>>>
AlphaBetaPlayer::QSearch(
    Stack* ss,
    NodeType node_type,
    ThreadState& thread_state,
    int depth,
    int alpha,
    int beta,
    bool maximizing_player,
    const std::optional<std::chrono::time_point<std::chrono::system_clock>>& deadline,
    PVInfo& pv_info) {
  Board& board = thread_state.GetBoard();
  if (canceled_
      || (deadline.has_value()
        && std::chrono::system_clock::now() >= *deadline)) {
    return std::nullopt;
  }
  if (depth < 0) {
    num_nodes_++;
  }

  bool is_pv_node = node_type != NonPV;
  int tt_depth = 0;

  std::optional<Move> tt_move;

  const HashTableEntry* tte = nullptr;
  if (options_.enable_transposition_table) {
    int64_t key = board.HashKey();

    tte = transposition_table_->Get(key);
    if (tte != nullptr) {
      if (tte->key == key) { // valid entry
        if (tte->depth >= tt_depth) {
          num_cache_hits_++;
          // at non-PV nodes check for an early TT cutoff
          if (!is_pv_node
              && (tte->bound == EXACT
                || (tte->bound == LOWER_BOUND && tte->score >= beta)
                || (tte->bound == UPPER_BOUND && tte->score <= alpha))
             ) {

            return std::make_tuple(
                std::min(beta, std::max(alpha, tte->score)), std::nullopt);
          }
        }
        tt_move = tte->move;
      }
    }

  }

  // get the current player to move
  Player player = board.GetTurn();

  // is the current player to move in check
  const bool in_check = board.IsKingInCheck(player);

  // is the partner of the current player to move in check
  const bool partner_checked = board.IsKingInCheck(GetPartner(player));
  
  // overall is the team in check
  const bool team_checked = in_check || partner_checked;
  
  // update the stack info
  ss->in_check = in_check;

  // initialize score
  int eval          =  value_none_tt;
  int best_value    = -kMateValue;
  int futility_base = -kMateValue;

  // check detection
  if (in_check) {
    // skip early pruning
    best_value = -kMateValue;

  } else {
    // standing pat
    if (tt_move.has_value() && tte->eval != value_none_tt) {
      best_value = eval = tte->eval;
    } else {
      best_value = eval = Evaluate(thread_state, maximizing_player, alpha, beta);
    }

    if (best_value >= beta) {
      if (options_.enable_transposition_table)
      {
        transposition_table_->Save(board.HashKey(), 0, std::nullopt, 0, best_value, LOWER_BOUND, is_pv_node);
      }

      return std::make_tuple(best_value, std::nullopt);
    }

    // delta pruning
    if (best_value + kPieceEvaluations[QUEEN] < alpha)
    {
      return std::make_tuple(alpha, std::nullopt);
    }

    // futility base
    futility_base = best_value;
  }

  std::optional<Move> best_move;
  int player_color = static_cast<int>(player.GetColor());

  int curr_n_activated = thread_state.NActivated()[player_color];
  int curr_total_moves = thread_state.TotalMoves()[player_color];

  const PieceToHistory* cont_hist[] = {
    (ss - 1)->continuation_history,
    (ss - 2)->continuation_history,
    (ss - 3)->continuation_history,
    (ss - 4)->continuation_history,
    (ss - 5)->continuation_history,
  };

  std::optional<Move> pv_move = pv_info.GetBestMove();
  Move* moves = thread_state.GetNextMoveBufferPartition();
  MovePicker move_picker(
    board,
    pv_move,
    ss->killers,
    kPieceEvaluations,
    thread_state.history_heuristic,
    thread_state.capture_heuristic,
    piece_move_order_scores_,
    options_.enable_move_order_checks,
    moves,
    kBufferPartitionSize
   , thread_state.counter_moves
   , /*include_quiets=*/in_check
   , cont_hist
    );

  int move_count = 0;
  int quiet_check_evasions = 0;
  bool fail_low = true;
  bool fail_high = false;
  std::vector<Move> searched_moves;

  while (true) {
    Move* move_ptr = move_picker.GetNextMove();
    if (move_ptr == nullptr) {
      break;
    }
    Move& move = *move_ptr;
    bool capture = move.IsCapture();
    if (!in_check) {
      if (capture) {
        if (move.GetStandardCapture().Present()) {
          // small optimization on SEE calculation
          if (move.GetCapturePiece().GetPieceType() != QUEEN
              && board.GetPiece(move.From()).GetPieceType() != PAWN) {
            int see = StaticExchangeEvaluationCapture(kPieceEvaluations, board, move);
            if (see < 0) {
              continue;
            }
          }
        }
      } else {
        continue;
      }
    }

    std::optional<std::tuple<int, std::optional<Move>>> value_and_move_or;

    PieceType piece_type = board.GetPiece(move.From()).GetPieceType();
    ss->current_move = move;
    ss->continuation_history = &thread_state.continuation_history[ss->in_check][move.IsCapture()][piece_type][move.To().GetRow()][move.To().GetCol()];

    bool delivers_check = move.DeliversCheck(board);
    board.MakeMove(move);
    if (board.CheckWasLastMoveKingCapture() != IN_PROGRESS) {
      board.UndoMove();

      best_value = beta; // fail hard
      best_move = move;
      pv_info.SetBestMove(move);
      break;
    }

    if (board.IsKingInCheck(player)) { // invalid move
      board.UndoMove();
      continue;
    }

    move_count++;

    bool is_pv_move = pv_move.has_value() && *pv_move == move;

    std::shared_ptr<PVInfo> child_pvinfo;
    if (is_pv_move && pv_info.GetChild() != nullptr) {
      child_pvinfo = pv_info.GetChild();
    } else {
      child_pvinfo = std::make_shared<PVInfo>();
    }

    // pruning
    if (best_value > -kMateValue) {
      if ((!delivers_check && move_count > 2)
          || quiet_check_evasions > 1) {
        board.UndoMove();
        continue;
      }
      if (move.IsCapture()
          && !delivers_check
          && futility_base + kPieceEvaluations[move.GetCapturePiece().GetPieceType()] < alpha) {
        board.UndoMove();
        continue;
      }
    }

    quiet_check_evasions += !capture && in_check;

    if (options_.enable_mobility_evaluation
        || options_.enable_piece_activation) {
      UpdateMobilityEvaluation(thread_state, player);
    }

    value_and_move_or = QSearch(
        ss+1, node_type, thread_state, depth - 1, -beta, -alpha, !maximizing_player,
        deadline, *child_pvinfo);

    board.UndoMove();

    if (options_.enable_mobility_evaluation
        || options_.enable_piece_activation) { // reset
      thread_state.NActivated()[player_color] = curr_n_activated;
      thread_state.TotalMoves()[player_color] = curr_total_moves;
    }

    if (!value_and_move_or.has_value()) {
      thread_state.ReleaseMoveBufferPartition();
      return std::nullopt; // timeout
    }
    int score = -std::get<0>(*value_and_move_or);
    searched_moves.push_back(move);

    if (!best_move.has_value()) {
      best_move = move;
      pv_info.SetChild(child_pvinfo);
      pv_info.SetBestMove(move);
    }
    if (score > best_value) {
      best_value = score;
      if (score > alpha) {
        fail_low = false;
        best_move = move;
        // update pv
        if (is_pv_node) {
          pv_info.SetChild(child_pvinfo);
          pv_info.SetBestMove(move);
        }
        if (score < beta) {
          alpha = score;
        } else {
          fail_high = true;
          break;  // fail high
        }
      }
    }
  }

  // check for fail low
  if (!fail_low)
  {
    UpdateStats(ss, thread_state, board, *best_move, /*depth=*/0, fail_high, searched_moves);
  }

  int score = best_value;

  // check for mate
  if (in_check && best_value == -kMateValue)
  {
    // checkmate
    score = std::min(beta, std::max(alpha, -kMateValue));
  }

  // update tt table
  if (options_.enable_transposition_table)
  {
    ScoreBound bound = beta <= alpha ? LOWER_BOUND : UPPER_BOUND;
    transposition_table_->Save(board.HashKey(), tt_depth, best_move, score, eval, bound, is_pv_node);
  }

  // relax the thread handler
  thread_state.ReleaseMoveBufferPartition();

  // return the best score
  return std::make_tuple(score, best_move);
}


void AlphaBetaPlayer::UpdateStats(
    Stack* ss, ThreadState& thread_state, const Board& board,
    const Move& move, int depth, bool fail_high,
    const std::vector<Move>& searched_moves) {
  auto from = move.From();
  auto to = move.To();
  Piece piece = board.GetPiece(move.From());

  int bonus = 1 << (fail_high ? depth + 1: depth);

  if (move.IsCapture()) {
    Piece captured = move.GetCapturePiece();
    thread_state.capture_heuristic[piece.GetPieceType()][piece.GetColor()]
      [captured.GetPieceType()][captured.GetColor()]
      [to.GetRow()][to.GetCol()] += bonus;
  } else {
    if (options_.enable_history_heuristic) {
      thread_state.history_heuristic[piece.GetPieceType()][from.GetRow()][from.GetCol()]
        [to.GetRow()][to.GetCol()] += bonus;
    }
    if (options_.enable_counter_move_heuristic) {
      thread_state.counter_moves[from.GetRow()*14*14*14 + from.GetCol()*14*14
        + to.GetRow()*14 + to.GetCol()] = move;
    }
    UpdateQuietStats(ss, move);
    UpdateContinuationHistories(ss, move, piece.GetPieceType(), bonus);
  }
  for (const auto& other_move : searched_moves) {
    if (other_move != move) {
      auto other_from = other_move.From();
      auto other_to = other_move.To();
      Piece other_piece = board.GetPiece(other_from);
      if (other_move.IsCapture()) {
        Piece other_captured = other_move.GetCapturePiece();
        thread_state.capture_heuristic[other_piece.GetPieceType()][other_piece.GetColor()]
          [other_captured.GetPieceType()][other_captured.GetColor()]
          [other_to.GetRow()][other_to.GetCol()] -= bonus;
      } else {
        thread_state.history_heuristic[other_piece.GetPieceType()][other_from.GetRow()][other_from.GetCol()]
          [other_to.GetRow()][other_to.GetCol()] -= bonus;
      }
    }
  }
}

void AlphaBetaPlayer::UpdateQuietStats(Stack* ss, const Move& move) {
  if (options_.enable_killers) {
    if (ss->killers[0] != move) {
      ss->killers[1] = ss->killers[0];
      ss->killers[0] = move;
    }
  }
}

void AlphaBetaPlayer::UpdateContinuationHistories(Stack* ss, const Move& move, PieceType piece_type, int bonus) {
  const auto to = move.To();
  for (int i : {1, 2, 3, 4, 5, 6}) {
    // Only update the first 2 continuation histories if we are in check
    if (ss->in_check && i > 2) {
      break;
    }
    if ((ss-i)->current_move.Present()) {
      (*(ss-i)->continuation_history)[piece_type][to.GetRow()][to.GetCol()] << bonus;
    }
  }
}

namespace {

constexpr int kPieceImbalanceTable[16] = {
  0, -25, -50, -150, -300, -350, -400, -400,
  -400, -400, -400, -400, -400, -400, -400, -400,
};

int GetNumMajorPieces(const std::vector<PlacedPiece>& pieces) {
  int num_major = 0;
  for (const auto& placed_piece : pieces) {
    PieceType pt = placed_piece.GetPiece().GetPieceType();
    if (pt != PAWN && pt != KING) {
      num_major++;
    }
  }
  return num_major;
}

}  // namespace

int AlphaBetaPlayer::Evaluate(
    ThreadState& thread_state, bool maximizing_player, int alpha, int beta) {
  int eval; // w.r.t. RY team
  Board& board = thread_state.GetBoard();
  GameResult game_result = board.CheckWasLastMoveKingCapture();
  if (game_result != IN_PROGRESS) { // game is over
    if (game_result == WIN_RY) {
      eval = kMateValue;
    } else if (game_result == WIN_BG) {
      eval = -kMateValue;
    } else {
      eval = 0; // stalemate
    }
  } else {
    // Piece evaluation
    eval = board.PieceEvaluation();

    auto threat_value = [](int t1, int t2) {
      constexpr int kThreatValue = 120;
      int threat = kThreatValue * (t1 + t2);
      return threat;
    };

    eval += threat_value(thread_state.n_threats[RED],
                         thread_state.n_threats[YELLOW]);
    eval -= threat_value(thread_state.n_threats[BLUE],
                         thread_state.n_threats[GREEN]);

    int n_queen_ry = 0;
    int n_queen_bg = 0;
    if (options_.enable_piece_square_table
        || options_.enable_knight_bonus) {
      const auto& piece_list = board.GetPieceList();
      for (int color = 0; color < 4; color++) {
        for (const auto& placed_piece : piece_list[color]) {
          PieceType piece_type = placed_piece.GetPiece().GetPieceType();
          const auto& loc = placed_piece.GetLocation();
          int row = loc.GetRow();
          int col = loc.GetCol();

          if (piece_type == QUEEN) {
            if (color == RED || color == YELLOW) {
              n_queen_ry++;
            } else {
              n_queen_bg++;
            }
          } else if (piece_type == PAWN) {
            int advancement = 0;
            switch (color) {
            case RED:
              advancement = 12 - row;
              break;
            case YELLOW:
              advancement = row - 1;
              break;
            case BLUE:
              advancement = col - 1;
              break;
            case GREEN:
              advancement = 12 - col;
              break;
            default:
              break;
            }
            int bonus = 2 * std::pow(advancement, 2);
            bonus += std::max(150 * (advancement - 5), 0);
            if (color == RED || color == YELLOW) {
              eval += bonus;
            } else {
              eval -= bonus;
            }
          } else if (piece_type == ROOK) {
            int rook_bonus = 0;
            constexpr int kRookBonus1 = 50;
            constexpr int kRookBonus2 = 25;
            if (col >= 4 && col <= 10 && row >= 4 && row <= 10) {
              rook_bonus = kRookBonus1;
            } else {
              int delta_row = color == RED ? -1 : color == YELLOW ? 1 : 0;
              int delta_col = color == BLUE ? 1 : color == GREEN ? -1: 0;
              int blocked_by_pawn = false;
              for (int i = 1; i < 7; i++) {
                int r = row + i * delta_row;
                int c = col + i * delta_col;
                if (board.IsLegalLocation(r, c)) {
                  const auto& other_piece = board.GetPiece(r, c);
                  if (other_piece.GetPieceType() == PAWN) {
                    blocked_by_pawn = true;
                    break;
                  }
                }
              }
              if (!blocked_by_pawn) {
                rook_bonus = kRookBonus2;
              }
            }

            if (color == RED || color == YELLOW) {
              eval += rook_bonus;
            } else {
              eval -= rook_bonus;
            }
          }

          if (options_.enable_piece_square_table) {
            if (color == RED || color == YELLOW) {
              eval += piece_square_table_[color][piece_type][row][col];
            } else {
              eval -= piece_square_table_[color][piece_type][row][col];
            }
          }

          // bonus for knights 2 moves away from enemy king
          if (options_.enable_knight_bonus
              && piece_type == KNIGHT) {
            int knight_bonus = 0;
            for (int i = 0; i < 2; i++) {
              PlayerColor other_color = static_cast<PlayerColor>(
                  (color + 2 * i + 1) % 4);
              auto king_loc = board.GetKingLocation(other_color);
              int king_row = king_loc.GetRow();
              int king_col = king_loc.GetCol();
              if (knight_to_king_[row][col][king_row][king_col]) {
                knight_bonus += 100;
              }
            }
            if (color == RED || color == YELLOW) {
              eval += knight_bonus;
            } else {
              eval -= knight_bonus;
            }
          }
        }
      }
    }

    int activation_ry = 0;
    int activation_bg = 0;
    if (options_.enable_piece_activation) {
      auto team_activation_score = [](int n_player1, int n_player2) {
        constexpr int A = 35;
        constexpr int B = 20;
        return A * (n_player1 + n_player2) + B * n_player1 * n_player2;
      };

      int* n_activated = thread_state.NActivated();
      activation_ry = team_activation_score(n_activated[RED], n_activated[YELLOW]);
      activation_bg = team_activation_score(n_activated[BLUE], n_activated[GREEN]);
      eval += activation_ry - activation_bg;
    }

    // Asymmetric evaluation for playing style.
    // If engine_team is NO_TEAM, then the eval is symmetric.
    constexpr int kAsymmetricQueenBonus = 0;
    constexpr int kStartEvaluation =
      16 * kPieceEvaluations[PAWN]
      + 4 * kPieceEvaluations[KNIGHT]
      + 4 * kPieceEvaluations[BISHOP]
      + 4 * kPieceEvaluations[ROOK]
      + 2 * kPieceEvaluations[QUEEN]
      + 2 * kPieceEvaluations[KING]
      ;
    constexpr float kAsymmetricPieceEvalFactor = 0.05f;
    constexpr float kAsymmetricActivationEvalFactor = 0.00;
    constexpr int kAsymmetricQueenBonus2 = 0.5 * kAsymmetricPieceEvalFactor * kPieceEvaluations[QUEEN];

    auto asym_eval = [&](
        int n_moves,
        int n_queen,
        int activation_eval,
        int player1_eval,
        int player2_eval) {
      int asym_eval = 0;
      asym_eval += n_queen * kAsymmetricQueenBonus;
      if (n_queen >= 2) {
        asym_eval += kAsymmetricQueenBonus2;
      }
      asym_eval += kAsymmetricActivationEvalFactor * activation_eval;
      asym_eval += kAsymmetricPieceEvalFactor * (player1_eval + player2_eval);
      asym_eval += n_moves/2;
      // subtract constant to make the score even at the start position
      asym_eval -= kAsymmetricQueenBonus * 2 + kAsymmetricQueenBonus2;
      asym_eval -= kAsymmetricPieceEvalFactor * kStartEvaluation;
      return asym_eval;
    };

    int* total_moves = thread_state.TotalMoves();
    if ((options_.engine_team == RED_YELLOW)
        || (options_.engine_team == CURRENT_TEAM
            && root_team_ == RED_YELLOW)) {
      eval += asym_eval(total_moves[RED] + total_moves[YELLOW],
          n_queen_ry, activation_ry,
          board.PieceEvaluation(RED), board.PieceEvaluation(YELLOW));
    } else if ((options_.engine_team == BLUE_GREEN)
               || (options_.engine_team == CURRENT_TEAM
                   && root_team_ == BLUE_GREEN)) {
      eval -= asym_eval(total_moves[BLUE] + total_moves[GREEN],
          n_queen_bg, activation_bg,
          board.PieceEvaluation(BLUE), board.PieceEvaluation(GREEN));
    }

    // double queen bonus
    constexpr int kMultiQueenBonus = 200;
    if (n_queen_ry >= 2) {
      eval += kMultiQueenBonus;
    }
    if (n_queen_bg >= 2) {
      eval -= kMultiQueenBonus;
    }

    // Mobility evaluation
    if (options_.enable_mobility_evaluation) {
      eval += 2 * (total_moves[RED] + total_moves[YELLOW]
                   - total_moves[BLUE] - total_moves[GREEN]);
    }

    auto lazy_skip = [&](int margin) {
      if (!options_.enable_lazy_eval) {
        return false;
      }
      int re = maximizing_player ? eval : -eval; // returned eval
      return re + margin <= alpha || re >= beta + margin;
    };

    if (options_.enable_piece_imbalance) {
      const auto& piece_list = board.GetPieceList();
      int n_major_red = GetNumMajorPieces(piece_list[RED]);
      int n_major_yellow = GetNumMajorPieces(piece_list[YELLOW]);
      int n_major_blue = GetNumMajorPieces(piece_list[BLUE]);
      int n_major_green = GetNumMajorPieces(piece_list[GREEN]);

      int diff_ry = std::abs(n_major_red - n_major_yellow);
      int diff_bg = std::abs(n_major_blue - n_major_green);

      eval += kPieceImbalanceTable[diff_ry] - kPieceImbalanceTable[diff_bg];
    }

    constexpr int kKingSafetyMargin = 600;
    if (lazy_skip(kKingSafetyMargin)) {
      num_lazy_eval_++;
      return maximizing_player ? eval : -eval;
    }

    // King safety evaluation (no lazy eval)
    if (options_.enable_king_safety) {
      for (int color = 0; color < 4; ++color) {
        int king_safety = 0;
        PlayerColor pl_cl = static_cast<PlayerColor>(color);
        Player player(pl_cl);
        Team team = player.GetTeam();
        //Team other = OtherTeam(team);
        const auto king_location = board.GetKingLocation(pl_cl);
        if (king_location.Present()) {

          bool opponent_has_queen =
            ((color == RED || color == YELLOW) && n_queen_bg > 0)
            || ((color == BLUE || color == GREEN) && n_queen_ry > 0);
          int safety = 0;

          if (options_.enable_pawn_shield
              && opponent_has_queen) {
            bool shield = HasShield(board, pl_cl, king_location);
            bool on_back_rank = OnBackRank(king_location);
            if (!shield) {
              safety -= 75;
            }
            if (!on_back_rank) {
              safety -= 50;
            }
            if (!shield && !on_back_rank) {
              safety -= 50;
            }
          }

          if (options_.enable_attacking_king_zone) {

            int num_attacker_colors = 0;
            int attacker_colors[4] = {0, 0, 0, 0};
            for (int delta_row = -1; delta_row <= 1; ++delta_row) {
              for (int delta_col = -1; delta_col <= 1; ++delta_col) {
                int row = king_location.GetRow() + delta_row;
                int col = king_location.GetCol() + delta_col;
                BoardLocation loc(row, col);
                if (!board.IsLegalLocation(loc) || OnBackRank(loc)) {
                  continue;
                }
                BoardLocation piece_location(row, col);

                PlacedPiece attackers[15];
                size_t num_pieces = board.GetAttackers2(attackers, 15, NO_TEAM, piece_location);

                if (num_pieces > 0) {
                  int value_of_attacks = 0;
                  int num_attackers = 0;
                  int value_of_protection = 0;
                  int num_protectors = 0;
                  for (size_t attacker_id = 0; attacker_id < num_pieces; attacker_id++) {
                    const auto& placed_piece = attackers[attacker_id];
                    const auto& piece = placed_piece.GetPiece();
                    if (piece.GetPieceType() == KING) {
                      continue;
                    }
                    int val = king_attacker_values_[piece.GetPieceType()];
                    if (piece.GetTeam() == team) {
                      num_protectors++;
                      value_of_protection += val;
                    } else {
                      num_attackers++;
                      value_of_attacks += val;
                      if (val > 0) {
                        attacker_colors[piece.GetColor()]++;
                      }
                    }
                  }
                  int attack_zone = value_of_attacks * king_attack_weight_[num_attackers] / 100;
                  attack_zone -= value_of_protection * king_attack_weight_[num_protectors] / 200;
                  attack_zone = std::max(attack_zone, 0);
                  safety -= attack_zone;
                }
              }
            }

            for (int i = 0; i < 4; i++) {
              if (attacker_colors[i] > 0) {
                num_attacker_colors++;
              }
            }
            if (num_attacker_colors > 1) {
              safety -= 150;
            }

            if (!opponent_has_queen) {
              safety /= 2;
            }

            safety = std::min(safety, 0);

            king_safety += safety;
          }

        }

        if (color == RED || color == YELLOW) {
          eval += king_safety;
        } else {
          eval -= king_safety;
        }

      }
    }

  }
  // w.r.t. maximizing team
  return maximizing_player ? eval : -eval;
}

void ThreadState::ResetHistoryHeuristic() {
  std::memset(history_heuristic, 0, (6*14*14*14*14) * sizeof(int) / sizeof(char));
  std::memset(capture_heuristic, 0, (6*4*6*4*14*14) * sizeof(int) / sizeof(char));

  for (bool in_check : {false, true}) {
    for (StatsType c : {NoCaptures, Captures}) {
      for (auto& to_row : continuation_history[in_check][c]) {
        for (auto& to_col : to_row) {
          for (auto& h : to_col) {
            h->fill(0);
          }
        }
      }
    }
  }
}

void AlphaBetaPlayer::ResetMobilityScores(ThreadState& thread_state) {
  // reset pseudo-mobility scores
  if (options_.enable_mobility_evaluation || options_.enable_piece_activation) {
    for (int i = 0; i < 4; i++) {
      Player player(static_cast<PlayerColor>(i));
      UpdateMobilityEvaluation(thread_state, player);
    }
  }
}

int AlphaBetaPlayer::StaticEvaluation(Board& board) {
  auto pv_copy = pv_info_.Copy();
  ThreadState thread_state(options_, board, *pv_copy);
  ResetMobilityScores(thread_state);
  return Evaluate(thread_state, true, -kMateValue, kMateValue);
}

std::optional<std::tuple<int, std::optional<Move>, int>>
AlphaBetaPlayer::MakeMove(
    Board& board,
    std::optional<std::chrono::milliseconds> time_limit,
    int max_depth) {
  root_team_ = board.GetTurn().GetTeam();
  int64_t hash_key = board.HashKey();
  if (hash_key != last_board_key_) {
    average_root_eval_ = 0;
    asp_nobs_ = 0;
    asp_sum_ = 0;
    asp_sum_sq_ = 0;
  }
  last_board_key_ = hash_key;

  SetCanceled(false);
  // Use Alpha-Beta search with iterative deepening
  std::optional<std::chrono::time_point<std::chrono::system_clock>> deadline;
  auto start = std::chrono::system_clock::now();
  if (time_limit.has_value()) {
    deadline = start + *time_limit;
  }

  if (options_.max_search_depth.has_value()) {
    max_depth = std::min(max_depth, *options_.max_search_depth);
  }

  int num_threads = 1;
  if (options_.enable_multithreading) {
    num_threads = options_.num_threads;
  }
  assert(num_threads >= 1);
  std::vector<ThreadState> thread_states;
  thread_states.reserve(num_threads);
  for (int i = 0; i < num_threads; i++) {
    auto pv_copy = pv_info_.Copy();
    thread_states.emplace_back(options_, board, *pv_copy);
    auto& thread_state = thread_states.back();
    ResetMobilityScores(thread_state);
    thread_state.ResetHistoryHeuristic();
  }

  std::mutex mutex;
  std::optional<std::tuple<int, std::optional<Move>, int>> res;

  std::vector<std::unique_ptr<std::thread>> threads;
  for (size_t i = 0; i < thread_states.size(); i++) {
    threads.push_back(std::make_unique<std::thread>([
      i, &thread_states, deadline, max_depth, this, &res, &mutex] {
      ThreadState& thread_state = thread_states[i];
      auto r = MakeMoveSingleThread(thread_state, deadline,
          max_depth);
      SetCanceled(true);
      std::lock_guard<std::mutex> lock(mutex);
      if (!res.has_value()) {
        res = r;
        pv_info_ = thread_state.GetPVInfo();
      }
    }));
  }

  for (auto& thread : threads) {
    thread->join();
  }

  SetCanceled(false);
  return res;
}

std::optional<std::tuple<int, std::optional<Move>, int>>
AlphaBetaPlayer::MakeMoveSingleThread(
    ThreadState& thread_state,
    std::optional<std::chrono::time_point<std::chrono::system_clock>> deadline,
    int max_depth) {
  Board& board = thread_state.GetBoard();
  PVInfo& pv_info = thread_state.GetPVInfo();

  int next_depth = std::min(1 + pv_info.GetDepth(), max_depth);
  std::optional<std::tuple<int, std::optional<Move>>> res;
  int alpha = -kMateValue;
  int beta = kMateValue;
  bool maximizing_player = board.TeamToPlay() == RED_YELLOW;
  int searched_depth = 0;
  Stack stack[kMaxPly + 10];
  Stack* ss = stack + 7;
  for (int i = 7; i > 0; i--) {
    (ss-i)->continuation_history = &thread_state.continuation_history[0][0][NO_PIECE][0][0];
  }

  if (options_.enable_aspiration_window) {

    while (next_depth <= max_depth) {
      std::optional<std::tuple<int, std::optional<Move>>> move_and_value;

      int prev = average_root_eval_;
      int delta = 50;
      if (asp_nobs_ > 0) {
        delta = 50 + std::sqrt((asp_sum_sq_ - asp_sum_*asp_sum_/asp_nobs_)/asp_nobs_);
      }

      alpha = std::max(prev - delta, -kMateValue);
      beta = std::min(prev + delta, kMateValue);
      int fail_cnt = 0;

      while (true) {
        move_and_value = Search(
            ss, Root, thread_state, 1, next_depth, alpha, beta, maximizing_player,
            0, deadline, pv_info);
        if (!move_and_value.has_value()) { // Hit deadline
          break;
        }
        int evaluation = std::get<0>(*move_and_value);
        if (asp_nobs_ == 0) {
          average_root_eval_ = evaluation;
        } else {
          average_root_eval_ = (2 * evaluation + average_root_eval_) / 3;
        }
        asp_nobs_++;
        asp_sum_ += evaluation;
        asp_sum_sq_ += evaluation * evaluation;

        if (std::abs(evaluation) == kMateValue) {
          break;
        }

        if (evaluation <= alpha) {
          beta = (alpha + beta) / 2;
          alpha = std::max(evaluation - delta, -kMateValue);
          ++fail_cnt;
        } else if (evaluation >= beta) {
          beta = std::min(evaluation + delta, kMateValue);
          ++fail_cnt;
        } else {
          break;
        }

        if (fail_cnt >= 5) {
          alpha = -kMateValue;
          beta = kMateValue;
        }

        delta += delta / 3;
      }

      if (!move_and_value.has_value()) { // Hit deadline
        break;
      }
      res = move_and_value;
      searched_depth = next_depth;
      next_depth++;
      int evaluation = std::get<0>(*move_and_value);
      if (std::abs(evaluation) == kMateValue) {
        break;  // Proven win/loss
      }
    }

  } else {

    while (next_depth <= max_depth) {
      std::optional<std::tuple<int, std::optional<Move>>> move_and_value;

      move_and_value = Search(
          ss, Root, thread_state, 1, next_depth, alpha, beta, maximizing_player,
          0, deadline, pv_info);

      if (!move_and_value.has_value()) { // Hit deadline
        break;
      }
      res = move_and_value;
      searched_depth = next_depth;
      next_depth++;
      int evaluation = std::get<0>(*move_and_value);
      if (std::abs(evaluation) == kMateValue) {
        break;  // Proven win/loss
      }
    }

  }

  if (res.has_value()) {
    int eval = std::get<0>(*res);
    if (!maximizing_player) {
      eval = -eval;
    }
    return std::make_tuple(eval, std::get<1>(*res), searched_depth);
  }

  return std::nullopt;
}

int PVInfo::GetDepth() const {
  if (best_move_.has_value()) {
    if (child_ == nullptr) {
      return 1;
    }
    return 1 + child_->GetDepth();
  }
  return 0;
}

void AlphaBetaPlayer::UpdateMobilityEvaluation(
    ThreadState& thread_state, Player player) {
  Board& board = thread_state.GetBoard();

  Move* moves = thread_state.GetNextMoveBufferPartition();
  Player curr_player = board.GetTurn();
  board.SetPlayer(player);
  size_t num_moves = board.GetPseudoLegalMoves2(
      moves, kBufferPartitionSize);
  int color = player.GetColor();
  thread_state.TotalMoves()[color] = num_moves;

  if (options_.enable_piece_activation) {
    auto piece_activated = [this](
        int color, PieceType piece_type,
        const BoardLocation& location, int n_moves) {
      if (piece_type == KNIGHT) {
        // activated so long as it's not on the back rank
        int row = location.GetRow();
        int col = location.GetCol();
        bool back_rank = (color == RED && row == 13)
                      || (color == YELLOW && row == 0)
                      || (color == BLUE && col == 0)
                      || (color == GREEN && col == 13);
        return !back_rank;
      }
      return n_moves >= piece_activation_threshold_[piece_type];
    };

    // note: this computation is dependent on the implementation of
    // Board::GetPseudoLegalMoves2, which adds all moves for a given
    // piece/location at a time.
    BoardLocation last_loc = BoardLocation::kNoLocation;
    PieceType last_piece_type = NO_PIECE;
    int n_pieces_activated = 0;
    int n_moves = 0;
    int n_threats = 0;
    for (size_t move_id = 0; move_id < num_moves; move_id++) {
      auto& move = moves[move_id];
      const auto& from = move.From();
      const auto& to = move.To();
      const auto& piece = board.GetPiece(from);
      PieceType piece_type = piece.GetPieceType();

      if (move.IsCapture()) {
        int see = move.ApproxSEE(board, kPieceEvaluations);
        if (see >= 100) {
          n_threats++;
        }
      }

      // don't count back rank squares in mobility / activation
      switch (piece.GetColor()) {
      case RED:
        if (to.GetRow() >= 12) {
          continue;
        }
        break;
      case YELLOW:
        if (to.GetRow() <= 1) {
          continue;
        }
        break;
      case BLUE:
        if (to.GetCol() <= 1) {
          continue;
        }
        break;
      case GREEN:
        if (to.GetCol() >= 12) {
          continue;
        }
        break;
      default:
        break;
      }

      if (piece_type == QUEEN || piece_type == ROOK || piece_type == BISHOP
          || piece_type == KNIGHT) {
        if (from != last_loc) {
          if (piece_activated(color, last_piece_type, last_loc, n_moves)) {
            n_pieces_activated++;
          }
          last_loc = from;
          last_piece_type = piece_type;
          n_moves = 0;
        }
        n_moves++;
      }
    }
    if (piece_activated(color, last_piece_type, last_loc, n_moves)) {
      n_pieces_activated++;
    }
    thread_state.NActivated()[color] = n_pieces_activated;
    thread_state.n_threats[color] = n_threats;
  }

  board.SetPlayer(curr_player);
  thread_state.ReleaseMoveBufferPartition();
}

bool AlphaBetaPlayer::OnBackRank(
    const BoardLocation& loc) {
  return loc.GetRow() == 0 || loc.GetRow() == 13 || loc.GetCol() == 0
    || loc.GetCol() == 13;
}

bool AlphaBetaPlayer::HasShield(
    Board& board, PlayerColor color, const BoardLocation& king_loc) {
  int row = king_loc.GetRow();
  int col = king_loc.GetCol();

  auto ray_blocked = [&](int delta_row, int delta_col) {
    for (int i = 0; i < 2; i++) {
      BoardLocation loc(row + delta_row * (i + 1), col + delta_col * (i + 1));
      if (!board.IsLegalLocation(loc)) {
        return true;
      }
      const auto piece = board.GetPiece(loc);
      if (piece.Present()
          && piece.GetColor() == color) {
        return true;
      }
    }
    return false;
  };

  bool has_shield = true;
  switch (color) {
  case RED:
    return ray_blocked(-1, -1) && ray_blocked(-1, 0) && ray_blocked(-1, 1);
  case BLUE:
    return ray_blocked(-1, 1) && ray_blocked(0, 1) && ray_blocked(1, 1);
  case YELLOW:
    return ray_blocked(1, -1) && ray_blocked(1, 0) && ray_blocked(1, 1);
  case GREEN:
    return ray_blocked(-1, -1) && ray_blocked(0, -1) && ray_blocked(1, -1);
  default:
    abort();
  }
  return has_shield;
}

std::shared_ptr<PVInfo> PVInfo::Copy() const {
  std::shared_ptr<PVInfo> copy = std::make_shared<PVInfo>();
  if (best_move_.has_value()) {
    copy->SetBestMove(*best_move_);
  }
  std::shared_ptr<PVInfo> child = child_;
  if (child != nullptr) {
    child = child->Copy();
  }
  copy->SetChild(child);
  return copy;
}

}  // namespace chess
