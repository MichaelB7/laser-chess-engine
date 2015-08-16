#include <algorithm>
#include <iostream>
#include <random>
#include "board.h"
#include "btables.h"

/**
 * @brief Our implementation of a xorshift generator as discovered by George
 * Marsaglia.
 * This specific implementation is not fully pseudorandom, but attempts to
 * create good magic number candidates by artifically increasing the number
 * of high bits.
 */
static uint64_t mseed = 0, mstate = 0;
uint64_t magicRNG() {
    // Use "y" to achieve a larger number of high bits
    uint64_t y = ((mstate << 57) | (mseed << 57)) >> 1;
    mstate ^= mseed >> 17;
    mstate ^= mstate << 3;

    uint64_t temp = mseed;
    mseed = mstate;
    mstate = temp;

    // But not too high, or they will overflow out once multiplied by the mask
    return (y | (mseed ^ mstate)) >> 1;
}

// Zobrist hashing table and the start position key, both initialized at startup
static uint64_t zobristTable[794];
static uint64_t startPosZobristKey = 0;

// Shift amounts for Dumb7fill
const int NORTH_SOUTH_FILL = 8;
const int EAST_WEST_FILL = 1;
const int NE_SW_FILL = 9;
const int NW_SE_FILL = 7;

// Dumb7fill methods, only used to initialize magic bitboard tables
uint64_t fillRayRight(uint64_t rayPieces, uint64_t empty, int shift);
uint64_t fillRayLeft(uint64_t rayPieces, uint64_t empty, int shift);

/**
 * @brief Stores the 4 values necessary to get a magic ray attack from a
 * specific square
 * @var table A pointer to the start of the array of attack sets for this square
 * @var mask The mask of relevant occupancy bits for this square
 * @var magic The magic 64-bit integer that maps the mask to the array index
 * @var shift The amount to shift by after multiplying mask by magic
 */
struct MagicInfo {
    uint64_t *table;
    uint64_t mask;
    uint64_t magic;
    int shift;
};

// Masks the relevant rook or bishop occupancy bits for magic bitboards
static uint64_t ROOK_MASK[64];
static uint64_t BISHOP_MASK[64];
// The full attack table containing all attack sets of bishops and rooks
static uint64_t *attackTable;
// The magic values for bishops, one for each square
static MagicInfo magicBishops[64];
// The magic values for rooks, one for each square
static MagicInfo magicRooks[64];

// Lookup table for all squares in a line between the from and to squares
static uint64_t inBetweenSqs[64][64];

uint64_t indexToMask64(int index, int nBits, uint64_t mask);
uint64_t ratt(int sq, uint64_t block);
uint64_t batt(int sq, uint64_t block);
int magicMap(uint64_t masked, uint64_t magic, int nBits);
uint64_t findMagic(int sq, int m, bool isBishop);

/**
 * @brief Initializes the tables and values necessary for magic bitboards.
 * We use the "fancy" approach.
 * https://chessprogramming.wikispaces.com/Magic+Bitboards
 */
void initMagicTables(uint64_t seed) {
    // An arbitrarily chosen random number generator and seed
    // The constant seed allows this process to be deterministic for optimization
    // and debugging.
    mstate = 74036198046ULL;
    mseed = seed;
    // Initialize the rook and bishop masks
    for (int i = 0; i < 64; i++) {
        // The relevant bits are everything except the edges
        // However, we don't want to remove the edge that we are on
        uint64_t relevantBits = ((~FILES[0] & ~FILES[7]) | FILES[i&7])
                              & ((~RANKS[0] & ~RANKS[7]) | RANKS[i>>3]);
        // The masks are rook and bishop attacks on an empty board
        ROOK_MASK[i] = ratt(i, 0) & relevantBits;
        BISHOP_MASK[i] = batt(i, 0) & relevantBits;
    }
    // The attack table has 107648 entries, found by summing the 2^(# relevant bits)
    // for all squares of both bishops and rooks
    attackTable = new uint64_t[107648];
    // Keeps track of the start location of attack set arrays
    int runningPtrLoc = 0;
    // Initialize bishop magic values
    for (int i = 0; i < 64; i++) {
        uint64_t *tableStart = attackTable;
        magicBishops[i].table = tableStart + runningPtrLoc;
        magicBishops[i].mask = BISHOP_MASK[i];
        magicBishops[i].magic = findMagic(i, NUM_BISHOP_BITS[i], true);
        magicBishops[i].shift = 64 - NUM_BISHOP_BITS[i];
        // We need 2^n array slots for a mask of n bits
        runningPtrLoc += 1 << NUM_BISHOP_BITS[i];
    }
    // Initialize rook magic values
    for (int i = 0; i < 64; i++) {
        uint64_t *tableStart = attackTable;
        magicRooks[i].table = tableStart + runningPtrLoc;
        magicRooks[i].mask = ROOK_MASK[i];
        magicRooks[i].magic = findMagic(i, NUM_ROOK_BITS[i], false);
        magicRooks[i].shift = 64 - NUM_ROOK_BITS[i];
        runningPtrLoc += 1 << NUM_ROOK_BITS[i];
    }
    // Set up the actual attack table, bishops first
    for (int sq = 0; sq < 64; sq++) {
        int nBits = NUM_BISHOP_BITS[sq];
        uint64_t mask = BISHOP_MASK[sq];
        // For each possible mask result
        for (int i = 0; i < (1 << nBits); i++) {
            // Find the pointer of where to store the attack sets
            uint64_t *attTableLoc = magicBishops[sq].table;
            // Find the actual masked bits from the mask index
            uint64_t occ = indexToMask64(i, nBits, mask);
            // Get the attack set for this masked occupancy
            uint64_t attSet = batt(sq, occ);
            // Do the mapping to get the location in the attack table where we
            // store the attack set
            int magicIndex = magicMap(occ, magicBishops[sq].magic, nBits);
            attTableLoc[magicIndex] = attSet;
        }
    }
    // Then rooks
    for (int sq = 0; sq < 64; sq++) {
        int nBits = NUM_ROOK_BITS[sq];
        uint64_t mask = ROOK_MASK[sq];
        for (int i = 0; i < (1 << nBits); i++) {
            uint64_t *attTableLoc = magicRooks[sq].table;
            uint64_t occ = indexToMask64(i, nBits, mask);
            uint64_t attSet = ratt(sq, occ);
            int magicIndex = magicMap(occ, magicRooks[sq].magic, nBits);
            attTableLoc[magicIndex] = attSet;
        }
    }
}

void initZobristTable() {
    std::mt19937_64 rng (61280152908);
    for (int i = 0; i < 794; i++)
        zobristTable[i] = rng();

    Board b;
    int *mailbox = b.getMailbox();
    b.initZobristKey(mailbox);
    startPosZobristKey = b.getZobristKey();
    delete[] mailbox;
}

int epVictimSquare(int victimColor, uint16_t file) {
    return 8 * (3 + victimColor) + file;
}

void initInBetweenTable() {
    for (int sq1 = 0; sq1 < 64; sq1++) {
        for (int sq2 = 0; sq2 < 64; sq2++) {
            uint64_t imaginaryRook = ratt(sq1, INDEX_TO_BIT[sq2]);
            if (imaginaryRook & INDEX_TO_BIT[sq2]) {
                uint64_t imaginaryRook2 = ratt(sq2, INDEX_TO_BIT[sq1]);
                inBetweenSqs[sq1][sq2] = imaginaryRook & imaginaryRook2;
            }
            else {
                uint64_t imaginaryBishop = batt(sq1, INDEX_TO_BIT[sq2]);
                if (imaginaryBishop & INDEX_TO_BIT[sq2]) {
                    uint64_t imaginaryBishop2 = batt(sq2, INDEX_TO_BIT[sq1]);
                    inBetweenSqs[sq1][sq2] = imaginaryBishop & imaginaryBishop2;
                }
                else
                    inBetweenSqs[sq1][sq2] = 0;
            }
        }
    }
}

/*
 * Performs a PERFT (performance test). Useful for testing/debugging
 * PERFT n counts the number of possible positions after n moves by either side,
 * ex. PERFT 4 = # of positions after 2 moves from each side
 * 
 * 7/8/15: PERFT 5, 1.46 s (i5-2450m)
 * 7/11/15: PERFT 5, 1.22 s (i5-2450m)
 * 7/13/15: PERFT 5, 1.08 s (i5-2450m)
 * 7/14/15: PERFT 5, 0.86 s (i5-2450m)
 * 7/17/15: PERFT 5, 0.32 s (i5-2450m)
 * 8/7/15: PERFT 5, 0.25 s, PERFT 6, 6.17 s (i5-5200u)
 * 8/8/15: PERFT 6, 5.90 s (i5-5200u)
 * 8/11/15: PERFT 6, 5.20 s (i5-5200u)
 */
uint64_t perft(Board &b, int color, int depth, uint64_t &captures) {
    if (depth == 0)
        return 1;

    uint64_t nodes = 0;

    MoveList pl = b.getPseudoLegalQuiets(color);
    for (unsigned int i = 0; i < pl.size(); i++) {
        Board copy = b.staticCopy();
        if (!copy.doPseudoLegalMove(pl.get(i), color))
            continue;

        nodes += perft(copy, color^1, depth-1, captures);
    }

    MoveList pc = b.getPseudoLegalCaptures(color, true);
    for (unsigned int i = 0; i < pc.size(); i++) {
        Board copy = b.staticCopy();
        if (!copy.doPseudoLegalMove(pc.get(i), color))
            continue;
        
        captures++;
        nodes += perft(copy, color^1, depth-1, captures);
    }
    return nodes;
}

// Create a board object initialized to the start position.
Board::Board() {
    allPieces[WHITE] = 0x000000000000FFFF;
    allPieces[BLACK] = 0xFFFF000000000000;
    pieces[WHITE][PAWNS] = 0x000000000000FF00; // white pawns
    pieces[WHITE][KNIGHTS] = 0x0000000000000042; // white knights
    pieces[WHITE][BISHOPS] = 0x0000000000000024; // white bishops
    pieces[WHITE][ROOKS] = 0x0000000000000081; // white rooks
    pieces[WHITE][QUEENS] = 0x0000000000000008; // white queens
    pieces[WHITE][KINGS] = 0x0000000000000010; // white kings
    pieces[BLACK][PAWNS] = 0x00FF000000000000; // black pawns
    pieces[BLACK][KNIGHTS] = 0x4200000000000000; // black knights
    pieces[BLACK][BISHOPS] = 0x2400000000000000; // black bishops
    pieces[BLACK][ROOKS] = 0x8100000000000000; // black rooks
    pieces[BLACK][QUEENS] = 0x0800000000000000; // black queens
    pieces[BLACK][KINGS] = 0x1000000000000000; // black kings

    twoFoldSqs = RESET_TWOFOLD;
    zobristKey = startPosZobristKey;
    epCaptureFile = NO_EP_POSSIBLE;
    playerToMove = WHITE;
    moveNumber = 1;
    castlingRights = WHITECASTLE | BLACKCASTLE;
    fiftyMoveCounter = 0;
}

// Create a board object from a mailbox of the current board state.
Board::Board(int *mailboxBoard, bool _whiteCanKCastle, bool _blackCanKCastle,
        bool _whiteCanQCastle, bool _blackCanQCastle,  uint16_t _epCaptureFile,
        int _fiftyMoveCounter, int _moveNumber, int _playerToMove) {
    // Initialize bitboards
    for (int i = 0; i < 12; i++) {
        pieces[i/6][i%6] = 0;
    }
    for (int i = 0; i < 64; i++) {
        if (0 <= mailboxBoard[i] && mailboxBoard[i] <= 11) {
            pieces[mailboxBoard[i]/6][mailboxBoard[i]%6] |= INDEX_TO_BIT[i];
        }
    }
    allPieces[WHITE] = 0;
    for (int i = 0; i < 6; i++)
        allPieces[WHITE] |= pieces[0][i];
    allPieces[BLACK] = 0;
    for (int i = 0; i < 6; i++)
        allPieces[BLACK] |= pieces[1][i];

    twoFoldSqs = RESET_TWOFOLD;
    epCaptureFile = _epCaptureFile;
    playerToMove = _playerToMove;
    moveNumber = _moveNumber;
    castlingRights = 0;
    if (_whiteCanKCastle)
        castlingRights |= WHITEKSIDE;
    if (_whiteCanQCastle)
        castlingRights |= WHITEQSIDE;
    if (_blackCanKCastle)
        castlingRights |= BLACKKSIDE;
    if (_blackCanQCastle)
        castlingRights |= BLACKQSIDE;
    fiftyMoveCounter = _fiftyMoveCounter;
    initZobristKey(mailboxBoard);
}

Board::~Board() {}

// Creates a copy of a board
// Private constructor used only with staticCopy()
Board::Board(Board *b) {
    allPieces[WHITE] = b->allPieces[WHITE];
    allPieces[BLACK] = b->allPieces[BLACK];
    for (int i = 0; i < 6; i++) {
        pieces[0][i] = b->pieces[0][i];
    }
    for (int i = 0; i < 6; i++) {
        pieces[1][i] = b->pieces[1][i];
    }

    twoFoldSqs = b->twoFoldSqs;
    zobristKey = b->zobristKey;
    epCaptureFile = b->epCaptureFile;
    playerToMove = b->playerToMove;
    moveNumber = b->moveNumber;
    castlingRights = b->castlingRights;
    fiftyMoveCounter = b->fiftyMoveCounter;
}

Board Board::staticCopy() {
    return Board(this);
}

/*
Board *Board::dynamicCopy() {
    return (new Board(this));
}
*/

void Board::doMove(Move m, int color) {
    int startSq = getStartSq(m);
    int endSq = getEndSq(m);
    int pieceID = getPieceOnSquare(color, startSq);

    // Update flag based elements of Zobrist key
    zobristKey ^= zobristTable[769 + castlingRights];
    zobristKey ^= zobristTable[785 + epCaptureFile];

    // Record current board position for two-fold repetition
    if (isCapture(m) || pieceID == PAWNS || isCastle(m)) {
        twoFoldSqs = RESET_TWOFOLD;
    }
    else {
        twoFoldSqs <<= 16;
        twoFoldSqs |= (startSq << 8) | endSq;
    }

    if (getPromotion(m)) {
        if (isCapture(m)) {
            int captureType = getPieceOnSquare(color^1, endSq);
            pieces[color][PAWNS] &= ~INDEX_TO_BIT[startSq];
            pieces[color][getPromotion(m)] |= INDEX_TO_BIT[endSq];
            pieces[color^1][captureType] &= ~INDEX_TO_BIT[endSq];

            allPieces[color] &= ~INDEX_TO_BIT[startSq];
            allPieces[color] |= INDEX_TO_BIT[endSq];
            allPieces[color^1] &= ~INDEX_TO_BIT[endSq];

            zobristKey ^= zobristTable[384*color + startSq];
            zobristKey ^= zobristTable[384*color + 64*getPromotion(m) + endSq];
            zobristKey ^= zobristTable[384*(color^1) + 64*captureType + endSq];
        }
        else {
            pieces[color][PAWNS] &= ~INDEX_TO_BIT[startSq];
            pieces[color][getPromotion(m)] |= INDEX_TO_BIT[endSq];

            allPieces[color] &= ~INDEX_TO_BIT[startSq];
            allPieces[color] |= INDEX_TO_BIT[endSq];

            zobristKey ^= zobristTable[384*color + startSq];
            zobristKey ^= zobristTable[384*color + 64*getPromotion(m) + endSq];
        }
        epCaptureFile = NO_EP_POSSIBLE;
        fiftyMoveCounter = 0;
    } // end promotion
    else if (isCapture(m)) {
        if (isEP(m)) {
            pieces[color][PAWNS] &= ~INDEX_TO_BIT[startSq];
            pieces[color][PAWNS] |= INDEX_TO_BIT[endSq];
            uint64_t epCaptureSq = INDEX_TO_BIT[epVictimSquare(color^1, epCaptureFile)];
            pieces[color^1][PAWNS] &= ~epCaptureSq;

            allPieces[color] &= ~INDEX_TO_BIT[startSq];
            allPieces[color] |= INDEX_TO_BIT[endSq];
            allPieces[color^1] &= ~epCaptureSq;

            int capSq = epVictimSquare(color^1, epCaptureFile);
            zobristKey ^= zobristTable[384*color + startSq];
            zobristKey ^= zobristTable[384*color + endSq];
            zobristKey ^= zobristTable[384*(color^1) + capSq];
        }
        else {
            int captureType = getPieceOnSquare(color^1, endSq);
            pieces[color][pieceID] &= ~INDEX_TO_BIT[startSq];
            pieces[color][pieceID] |= INDEX_TO_BIT[endSq];
            pieces[color^1][captureType] &= ~INDEX_TO_BIT[endSq];

            allPieces[color] &= ~INDEX_TO_BIT[startSq];
            allPieces[color] |= INDEX_TO_BIT[endSq];
            allPieces[color^1] &= ~INDEX_TO_BIT[endSq];

            zobristKey ^= zobristTable[384*color + 64*pieceID + startSq];
            zobristKey ^= zobristTable[384*color + 64*pieceID + endSq];
            zobristKey ^= zobristTable[384*(color^1) + 64*captureType + endSq];
        }
        epCaptureFile = NO_EP_POSSIBLE;
        fiftyMoveCounter = 0;
    } // end capture
    else { // Quiet moves
        if (isCastle(m)) {
            if (endSq == 6) { // white kside
                pieces[WHITE][KINGS] &= ~INDEX_TO_BIT[4];
                pieces[WHITE][KINGS] |= INDEX_TO_BIT[6];
                pieces[WHITE][ROOKS] &= ~INDEX_TO_BIT[7];
                pieces[WHITE][ROOKS] |= INDEX_TO_BIT[5];

                allPieces[WHITE] &= ~INDEX_TO_BIT[4];
                allPieces[WHITE] |= INDEX_TO_BIT[6];
                allPieces[WHITE] &= ~INDEX_TO_BIT[7];
                allPieces[WHITE] |= INDEX_TO_BIT[5];

                zobristKey ^= zobristTable[64*KINGS+4];
                zobristKey ^= zobristTable[64*KINGS+6];
                zobristKey ^= zobristTable[64*ROOKS+7];
                zobristKey ^= zobristTable[64*ROOKS+5];
            }
            else if (endSq == 2) { // white qside
                pieces[WHITE][KINGS] &= ~INDEX_TO_BIT[4];
                pieces[WHITE][KINGS] |= INDEX_TO_BIT[2];
                pieces[WHITE][ROOKS] &= ~INDEX_TO_BIT[0];
                pieces[WHITE][ROOKS] |= INDEX_TO_BIT[3];

                allPieces[WHITE] &= ~INDEX_TO_BIT[4];
                allPieces[WHITE] |= INDEX_TO_BIT[2];
                allPieces[WHITE] &= ~INDEX_TO_BIT[0];
                allPieces[WHITE] |= INDEX_TO_BIT[3];

                zobristKey ^= zobristTable[64*KINGS+4];
                zobristKey ^= zobristTable[64*KINGS+2];
                zobristKey ^= zobristTable[64*ROOKS+0];
                zobristKey ^= zobristTable[64*ROOKS+3];
            }
            else if (endSq == 62) { // black kside
                pieces[BLACK][KINGS] &= ~INDEX_TO_BIT[60];
                pieces[BLACK][KINGS] |= INDEX_TO_BIT[62];
                pieces[BLACK][ROOKS] &= ~INDEX_TO_BIT[63];
                pieces[BLACK][ROOKS] |= INDEX_TO_BIT[61];

                allPieces[BLACK] &= ~INDEX_TO_BIT[60];
                allPieces[BLACK] |= INDEX_TO_BIT[62];
                allPieces[BLACK] &= ~INDEX_TO_BIT[63];
                allPieces[BLACK] |= INDEX_TO_BIT[61];

                zobristKey ^= zobristTable[384+64*KINGS+60];
                zobristKey ^= zobristTable[384+64*KINGS+62];
                zobristKey ^= zobristTable[384+64*ROOKS+63];
                zobristKey ^= zobristTable[384+64*ROOKS+61];
            }
            else { // black qside
                pieces[BLACK][KINGS] &= ~INDEX_TO_BIT[60];
                pieces[BLACK][KINGS] |= INDEX_TO_BIT[58];
                pieces[BLACK][ROOKS] &= ~INDEX_TO_BIT[56];
                pieces[BLACK][ROOKS] |= INDEX_TO_BIT[59];

                allPieces[BLACK] &= ~INDEX_TO_BIT[60];
                allPieces[BLACK] |= INDEX_TO_BIT[58];
                allPieces[BLACK] &= ~INDEX_TO_BIT[56];
                allPieces[BLACK] |= INDEX_TO_BIT[59];

                zobristKey ^= zobristTable[384+64*KINGS+60];
                zobristKey ^= zobristTable[384+64*KINGS+58];
                zobristKey ^= zobristTable[384+64*ROOKS+56];
                zobristKey ^= zobristTable[384+64*ROOKS+59];
            }
            epCaptureFile = NO_EP_POSSIBLE;
            fiftyMoveCounter++;
        } // end castling
        else { // other quiet moves
            pieces[color][pieceID] &= ~INDEX_TO_BIT[startSq];
            pieces[color][pieceID] |= INDEX_TO_BIT[endSq];

            allPieces[color] &= ~INDEX_TO_BIT[startSq];
            allPieces[color] |= INDEX_TO_BIT[endSq];

            zobristKey ^= zobristTable[384*color + 64*pieceID + startSq];
            zobristKey ^= zobristTable[384*color + 64*pieceID + endSq];

            // check for en passant
            if (pieceID == PAWNS) {
                if (getFlags(m) == MOVE_DOUBLE_PAWN)
                    epCaptureFile = startSq & 7;
                else
                    epCaptureFile = NO_EP_POSSIBLE;

                fiftyMoveCounter = 0;
            }
            else {
                epCaptureFile = NO_EP_POSSIBLE;
                fiftyMoveCounter++;
            }
        } // end other quiet moves
    } // end normal move

    // change castling flags
    if (pieceID == KINGS) {
        if (color == WHITE)
            castlingRights &= ~WHITECASTLE;
        else
            castlingRights &= ~BLACKCASTLE;
    }
    // Castling rights change because of the rook only when the rook moves or
    // is captured
    else if (isCapture(m) || pieceID == ROOKS) {
        // No sense in remove the rights if they're already gone
        if (castlingRights & WHITECASTLE) {
            if ((pieces[WHITE][ROOKS] & 0x80) == 0)
                castlingRights &= ~WHITEKSIDE;
            if ((pieces[WHITE][ROOKS] & 1) == 0)
                castlingRights &= ~WHITEQSIDE;
        }
        if (castlingRights & BLACKCASTLE) {
            uint64_t blackR = pieces[BLACK][ROOKS] >> 56;
            if ((blackR & 0x80) == 0)
                castlingRights &= ~BLACKKSIDE;
            if ((blackR & 0x1) == 0)
                castlingRights &= ~BLACKQSIDE;
        }
    } // end castling flags

    zobristKey ^= zobristTable[769 + castlingRights];
    zobristKey ^= zobristTable[785 + epCaptureFile];

    if (color == BLACK)
        moveNumber++;
    playerToMove = color^1;
    zobristKey ^= zobristTable[768];
}

bool Board::doPseudoLegalMove(Move m, int color) {
    doMove(m, color);
    // Pseudo-legal moves require a check for legality
    return !(isInCheck(color));
}

// Do a hash move, which requires a few more checks in case of a Type-1 error.
bool Board::doHashMove(Move m, int color) {
    int pieceID = getPieceOnSquare(color, getStartSq(m));
    // Check that the correct piece is on the start square
    if (!(pieces[color][pieceID] & INDEX_TO_BIT[getStartSq(m)]))
        return false;
    // Check that the end square has correct occupancy
    uint64_t otherPieces = allPieces[color^1];
    uint64_t endSingle = INDEX_TO_BIT[getEndSq(m)];
    bool captureRoutes = (isCapture(m) && (otherPieces & endSingle))
                      || (isCapture(m) && pieceID == PAWNS && (~otherPieces & endSingle));
    uint64_t empty = ~getOccupancy();
    if (!(captureRoutes || (!isCapture(m) && (empty & endSingle))))
        return false;

    return doPseudoLegalMove(m, color);
}

// Handle null moves for null move pruning by switching the player to move.
void Board::doNullMove() {
    playerToMove = playerToMove ^ 1;
    zobristKey ^= zobristTable[768];
}

// Get all legal moves and captures
MoveList Board::getAllLegalMoves(int color) {
    MoveList moves = getAllPseudoLegalMoves(color);

    for (unsigned int i = 0; i < moves.size(); i++) {
        Board b = staticCopy();
        b.doMove(moves.get(i), color);

        if (b.isInCheck(color)) {
            moves.remove(i);
            i--;
        }
    }

    return moves;
}

//------------------------------Pseudo-legal Moves------------------------------
/* Pseudo-legal moves disregard whether the player's king is left in check
 * The pseudo-legal move and capture generators all follow a similar scheme:
 * Bitscan to obtain the square number for each piece (a1 is 0, a2 is 1, h8 is 63).
 * Get the legal moves as a bitboard, then bitscan this to get the destination
 * square and store as a Move object.
 */
MoveList Board::getAllPseudoLegalMoves(int color) {
    MoveList quiets = getPseudoLegalQuiets(color);
    MoveList captures = getPseudoLegalCaptures(color, true);

    // Put captures before quiet moves
    for (unsigned int i = 0; i < quiets.size(); i++) {
        captures.add(quiets.get(i));
    }
    return captures;
}

/*
 * Quiet moves are generated in the following order:
 * Castling
 * Knight moves
 * Bishop moves
 * Rook moves
 * Queen moves
 * Pawn moves
 * King moves
 */
MoveList Board::getPseudoLegalQuiets(int color) {
    MoveList quiets;

    addCastlesToList(quiets, color);

    uint64_t knights = pieces[color][KNIGHTS];
    while (knights) {
        int stsq = bitScanForward(knights);
        knights &= knights-1;
        uint64_t nSq = getKnightSquares(stsq);

        addMovesToList(quiets, stsq, nSq, false);
    }

    uint64_t occ = getOccupancy();
    uint64_t bishops = pieces[color][BISHOPS];
    while (bishops) {
        int stsq = bitScanForward(bishops);
        bishops &= bishops-1;
        uint64_t bSq = getBishopSquares(stsq, occ);

        addMovesToList(quiets, stsq, bSq, false);
    }

    uint64_t rooks = pieces[color][ROOKS];
    while (rooks) {
        int stsq = bitScanForward(rooks);
        rooks &= rooks-1;
        uint64_t rSq = getRookSquares(stsq, occ);

        addMovesToList(quiets, stsq, rSq, false);
    }

    uint64_t queens = pieces[color][QUEENS];
    while (queens) {
        int stsq = bitScanForward(queens);
        queens &= queens-1;
        uint64_t qSq = getQueenSquares(stsq, occ);

        addMovesToList(quiets, stsq, qSq, false);
    }

    addPawnMovesToList(quiets, color);

    int stsqK = bitScanForward(pieces[color][KINGS]);
    uint64_t kingSqs = getKingSquares(stsqK);
    addMovesToList(quiets, stsqK, kingSqs, false);

    return quiets;
}

/*
 * Do not include promotions for quiescence search, include promotions in normal search.
 * Captures are generated in the following order:
 * King captures
 * Pawn captures
 * Knight captures
 * Bishop captures
 * Rook captures
 * Queen captures
 */
MoveList Board::getPseudoLegalCaptures(int color, bool includePromotions) {
    MoveList captures;

    uint64_t otherPieces = allPieces[color^1];

    int kingStSq = bitScanForward(pieces[color][KINGS]);
    uint64_t kingSqs = getKingSquares(kingStSq);
    addMovesToList(captures, kingStSq, kingSqs, true, otherPieces);

    addPawnCapturesToList(captures, color, otherPieces, includePromotions);

    uint64_t knights = pieces[color][KNIGHTS];
    while (knights) {
        int stsq = bitScanForward(knights);
        knights &= knights-1;
        uint64_t nSq = getKnightSquares(stsq);

        addMovesToList(captures, stsq, nSq, true, otherPieces);
    }

    uint64_t occ = getOccupancy();
    uint64_t bishops = pieces[color][BISHOPS];
    while (bishops) {
        int stsq = bitScanForward(bishops);
        bishops &= bishops-1;
        uint64_t bSq = getBishopSquares(stsq, occ);

        addMovesToList(captures, stsq, bSq, true, otherPieces);
    }

    uint64_t rooks = pieces[color][ROOKS];
    while (rooks) {
        int stsq = bitScanForward(rooks);
        rooks &= rooks-1;
        uint64_t rSq = getRookSquares(stsq, occ);

        addMovesToList(captures, stsq, rSq, true, otherPieces);
    }

    uint64_t queens = pieces[color][QUEENS];
    while (queens) {
        int stsq = bitScanForward(queens);
        queens &= queens-1;
        uint64_t qSq = getQueenSquares(stsq, occ);

        addMovesToList(captures, stsq, qSq, true, otherPieces);
    }

    return captures;
}

// Generates all queen promotions for quiescence search
MoveList Board::getPseudoLegalPromotions(int color) {
    MoveList moves;
    uint64_t otherPieces = allPieces[color^1];

    uint64_t pawns = pieces[color][PAWNS];
    uint64_t finalRank = (color == WHITE) ? RANKS[7] : RANKS[0];

    int leftDiff = (color == WHITE) ? -7 : 9;
    int rightDiff = (color == WHITE) ? -9 : 7;

    uint64_t legal = (color == WHITE) ? getWPawnLeftCaptures(pawns)
                                      : getBPawnLeftCaptures(pawns);
    legal &= otherPieces;
    uint64_t promotions = legal & finalRank;

    while (promotions) {
        int endSq = bitScanForward(promotions);
        promotions &= promotions-1;

        Move mq = encodeMove(endSq+leftDiff, endSq);
        mq = setCapture(mq, true);
        mq = setPromotion(mq, QUEENS);
        moves.add(mq);
    }

    legal = (color == WHITE) ? getWPawnRightCaptures(pawns)
                             : getBPawnRightCaptures(pawns);
    legal &= otherPieces;
    promotions = legal & finalRank;

    while (promotions) {
        int endSq = bitScanForward(promotions);
        promotions &= promotions-1;

        Move mq = encodeMove(endSq+rightDiff, endSq);
        mq = setCapture(mq, true);
        mq = setPromotion(mq, QUEENS);
        moves.add(mq);
    }

    int sqDiff = (color == WHITE) ? -8 : 8;

    legal = (color == WHITE) ? getWPawnSingleMoves(pawns)
                             : getBPawnSingleMoves(pawns);
    promotions = legal & finalRank;

    while (promotions) {
        int endSq = bitScanForward(promotions);
        promotions &= promotions - 1;
        int stSq = endSq + sqDiff;

        Move mq = encodeMove(stSq, endSq);
        mq = setPromotion(mq, QUEENS);
        moves.add(mq);
    }

    return moves;
}

/*
 * Get all pseudo-legal checks for a position. Used in quiescence search.
 * This function can be optimized compared to a normal getLegalMoves() because
 * for each piece, we can intersect the legal moves of the piece with the attack
 * map of this piece to the opposing king square to determine direct checks.
 * Discovered checks have to be handled separately.
 *
 * For simplicity, promotions and en passant are left out of this function.
 */
MoveList Board::getPseudoLegalChecks(int color) {
    MoveList checkCaptures, checks;
    int kingSq = bitScanForward(pieces[color^1][KINGS]);
    // Square parity for knight and bishop moves
    uint64_t kingParity = (pieces[color^1][KINGS] & LIGHT) ? LIGHT : DARK;
    uint64_t otherPieces = allPieces[color^1];
    uint64_t potentialXRay = pieces[color][BISHOPS] | pieces[color][ROOKS] | pieces[color][QUEENS];

    // We can do pawns in parallel, since the start square of a pawn move is
    // determined by its end square.
    uint64_t pawns = pieces[color][PAWNS];

    // First, deal with discovered checks
    // TODO this is way too slow
    /*
    uint64_t tempPawns = pawns;
    while (tempPawns) {
        int stsq = bitScanForward(tempPawns);
        tempPawns &= tempPawns - 1;
        uint64_t xrays = getXRayPieceMap(color, kingSq, color, INDEX_TO_BIT[stsq]);
        // If moving the pawn caused a new xray piece to attack the king
        if (!(xrays & invAttackMap)) {
            // Every legal move of this pawn is a legal check
            uint64_t legal = (color == WHITE) ? getWPawnSingleMoves(INDEX_TO_BIT[stsq]) | getWPawnDoubleMoves(INDEX_TO_BIT[stsq])
                                              : getBPawnSingleMoves(INDEX_TO_BIT[stsq]) | getBPawnDoubleMoves(INDEX_TO_BIT[stsq]);
            while (legal) {
                int endsq = bitScanForward(legal);
                legal &= legal - 1;
                checks.add(encodeMove(stsq, endsq, PAWNS, false));
            }

            legal = (color == WHITE) ? getWPawnLeftCaptures(INDEX_TO_BIT[stsq]) | getWPawnRightCaptures(INDEX_TO_BIT[stsq])
                                     : getBPawnLeftCaptures(INDEX_TO_BIT[stsq]) | getBPawnRightCaptures(INDEX_TO_BIT[stsq]);
            while (legal) {
                int endsq = bitScanForward(legal);
                legal &= legal - 1;
                checkCaptures.add(encodeMove(stsq, endsq, PAWNS, true));
            }
            // Remove this pawn from future consideration
            pawns ^= INDEX_TO_BIT[stsq];
        }
    }
    */

    uint64_t pAttackMap = (color == WHITE) 
            ? getBPawnLeftCaptures(INDEX_TO_BIT[kingSq]) | getBPawnRightCaptures(INDEX_TO_BIT[kingSq])
            : getWPawnLeftCaptures(INDEX_TO_BIT[kingSq]) | getWPawnRightCaptures(INDEX_TO_BIT[kingSq]);
    uint64_t finalRank = (color == WHITE) ? RANKS[7] : RANKS[0];
    int sqDiff = (color == WHITE) ? -8 : 8;

    uint64_t pLegal = (color == WHITE) ? getWPawnSingleMoves(pawns)
                                       : getBPawnSingleMoves(pawns);
    // Remove promotions
    uint64_t promotions = pLegal & finalRank;
    pLegal ^= promotions;

    pLegal &= pAttackMap;
    while (pLegal) {
        int endsq = bitScanForward(pLegal);
        pLegal &= pLegal - 1;
        checks.add(encodeMove(endsq+sqDiff, endsq));
    }

    pLegal = (color == WHITE) ? getWPawnDoubleMoves(pawns)
                              : getBPawnDoubleMoves(pawns);
    pLegal &= pAttackMap;
    while (pLegal) {
        int endsq = bitScanForward(pLegal);
        pLegal &= pLegal - 1;
        Move m = encodeMove(endsq+2*sqDiff, endsq);
        m = setFlags(m, MOVE_DOUBLE_PAWN);
        checks.add(m);
    }

    // For pawn captures, we can use a similar approach, but we must consider
    // left-hand and right-hand captures separately so we can tell which
    // pawn is doing the capturing.
    int leftDiff = (color == WHITE) ? -7 : 9;
    int rightDiff = (color == WHITE) ? -9 : 7;

    pLegal = (color == WHITE) ? getWPawnLeftCaptures(pawns)
                              : getBPawnLeftCaptures(pawns);
    pLegal &= otherPieces;
    promotions = pLegal & finalRank;
    pLegal ^= promotions;
    pLegal &= pAttackMap;

    while (pLegal) {
        int endsq = bitScanForward(pLegal);
        pLegal &= pLegal-1;
        Move m = encodeMove(endsq+leftDiff, endsq);
        m = setCapture(m, true);
        checkCaptures.add(m);
    }

    pLegal = (color == WHITE) ? getWPawnRightCaptures(pawns)
                              : getBPawnRightCaptures(pawns);
    pLegal &= otherPieces;
    promotions = pLegal & finalRank;
    pLegal ^= promotions;
    pLegal &= pAttackMap;

    while (pLegal) {
        int endsq = bitScanForward(pLegal);
        pLegal &= pLegal-1;
        Move m = encodeMove(endsq+rightDiff, endsq);
        m = setCapture(m, true);
        checkCaptures.add(m);
    }

    uint64_t knights = pieces[color][KNIGHTS] & kingParity;
    uint64_t nAttackMap = getKnightSquares(kingSq);
    while (knights) {
        int stsq = bitScanForward(knights);
        knights &= knights-1;
        uint64_t nSq = getKnightSquares(stsq);
        // Get any bishops, rooks, queens attacking king after knight has moved
        uint64_t xrays = getXRayPieceMap(color, kingSq, color, INDEX_TO_BIT[stsq], 0);
        // If still no xrayers are giving check, then we have no discovered
        // check. Otherwise, every move by this piece is a (discovered) checking
        // move
        if (!(xrays & potentialXRay))
            nSq &= nAttackMap;

        addMovesToList(checks, stsq, nSq, false);
        addMovesToList(checkCaptures, stsq, nSq, true, otherPieces);
    }

    uint64_t occ = getOccupancy();
    uint64_t bishops = pieces[color][BISHOPS] & kingParity;
    uint64_t bAttackMap = getBishopSquares(kingSq, occ);
    while (bishops) {
        int stsq = bitScanForward(bishops);
        bishops &= bishops-1;
        uint64_t bSq = getBishopSquares(stsq, occ);
        uint64_t xrays = getXRayPieceMap(color, kingSq, color, INDEX_TO_BIT[stsq], 0);
        if (!(xrays & potentialXRay))
            bSq &= bAttackMap;

        addMovesToList(checks, stsq, bSq, false);
        addMovesToList(checkCaptures, stsq, bSq, true, otherPieces);
    }

    uint64_t rooks = pieces[color][ROOKS];
    uint64_t rAttackMap = getRookSquares(kingSq, occ);
    while (rooks) {
        int stsq = bitScanForward(rooks);
        rooks &= rooks-1;
        uint64_t rSq = getRookSquares(stsq, occ);
        uint64_t xrays = getXRayPieceMap(color, kingSq, color, INDEX_TO_BIT[stsq], 0);
        if (!(xrays & potentialXRay))
            rSq &= rAttackMap;

        addMovesToList(checks, stsq, rSq, false);
        addMovesToList(checkCaptures, stsq, rSq, true, otherPieces);
    }

    uint64_t queens = pieces[color][QUEENS];
    uint64_t qAttackMap = getQueenSquares(kingSq, occ);
    while (queens) {
        int stsq = bitScanForward(queens);
        queens &= queens-1;
        uint64_t qSq = getQueenSquares(stsq, occ) & qAttackMap;

        addMovesToList(checks, stsq, qSq, false);
        addMovesToList(checkCaptures, stsq, qSq, true, otherPieces);
    }

    // Put captures before quiet moves
    for (unsigned int i = 0; i < checks.size(); i++) {
        checkCaptures.add(checks.get(i));
    }
    return checkCaptures;
}

// Generate moves that (sort of but not really) get out of check
// This can only be used if we know the side to move is in check
// Optimizations include looking for double check (king moves only),
// otherwise we can only capture the checker or block if it is an xray piece
MoveList Board::getPseudoLegalCheckEscapes(int color) {
    MoveList captures, blocks;

    int kingSq = bitScanForward(pieces[color][KINGS]);
    uint64_t otherPieces = allPieces[color^1];
    uint64_t attackMap = getAttackMap(color^1, kingSq);
    // Consider only captures of pieces giving check
    otherPieces &= attackMap;

    // If double check, we can only move the king
    if (count(otherPieces) >= 2) {
        uint64_t kingSqs = getKingSquares(kingSq);

        addMovesToList(captures, kingSq, kingSqs, true, allPieces[color^1]);
        addMovesToList(captures, kingSq, kingSqs, false);
        return captures;
    }

    addPawnMovesToList(blocks, color);
    addPawnCapturesToList(captures, color, otherPieces, true);

    uint64_t occ = getOccupancy();
    // If bishops, rooks, or queens, get bitboard of attack path so we
    // can intersect with legal moves to get legal block moves
    uint64_t xraySqs = 0;
    int attackerSq = bitScanForward(otherPieces);
    int attackerType = getPieceOnSquare(color^1, attackerSq);
    if (attackerType == BISHOPS)
        xraySqs = getBishopSquares(attackerSq, occ);
    else if (attackerType == ROOKS)
        xraySqs = getRookSquares(attackerSq, occ);
    else if (attackerType == QUEENS)
        xraySqs = getQueenSquares(attackerSq, occ);

    uint64_t knights = pieces[color][KNIGHTS];
    while (knights) {
        int stsq = bitScanForward(knights);
        knights &= knights-1;
        uint64_t nSq = getKnightSquares(stsq);

        addMovesToList(blocks, stsq, nSq & xraySqs, false);
        addMovesToList(captures, stsq, nSq, true, otherPieces);
    }

    uint64_t bishops = pieces[color][BISHOPS];
    while (bishops) {
        int stsq = bitScanForward(bishops);
        bishops &= bishops-1;
        uint64_t bSq = getBishopSquares(stsq, occ);

        addMovesToList(blocks, stsq, bSq & xraySqs, false);
        addMovesToList(captures, stsq, bSq, true, otherPieces);
    }

    uint64_t rooks = pieces[color][ROOKS];
    while (rooks) {
        int stsq = bitScanForward(rooks);
        rooks &= rooks-1;
        uint64_t rSq = getRookSquares(stsq, occ);

        addMovesToList(blocks, stsq, rSq & xraySqs, false);
        addMovesToList(captures, stsq, rSq, true, otherPieces);
    }

    uint64_t queens = pieces[color][QUEENS];
    while (queens) {
        int stsq = bitScanForward(queens);
        queens &= queens-1;
        uint64_t qSq = getQueenSquares(stsq, occ);

        addMovesToList(blocks, stsq, qSq & xraySqs, false);
        addMovesToList(captures, stsq, qSq, true, otherPieces);
    }

    int stsqK = bitScanForward(pieces[color][KINGS]);
    uint64_t kingSqs = getKingSquares(stsqK);
    addMovesToList(blocks, stsqK, kingSqs, false);
    addMovesToList(captures, stsqK, kingSqs, true, allPieces[color^1]);

    // Put captures before blocking moves
    for (unsigned int i = 0; i < blocks.size(); i++) {
        captures.add(blocks.get(i));
    }

    return captures;
}

//------------------------------------------------------------------------------
//---------------------------Move Generation Helpers----------------------------
//------------------------------------------------------------------------------
// We can do pawns in parallel, since the start square of a pawn move is
// determined by its end square.
void Board::addPawnMovesToList(MoveList &quiets, int color) {
    uint64_t pawns = pieces[color][PAWNS];
    uint64_t finalRank = (color == WHITE) ? RANKS[7] : RANKS[0];
    int sqDiff = (color == WHITE) ? -8 : 8;

    uint64_t pLegal = (color == WHITE) ? getWPawnSingleMoves(pawns)
                                       : getBPawnSingleMoves(pawns);
    // Promotions occur when a pawn reaches the final rank
    uint64_t promotions = pLegal & finalRank;
    pLegal ^= promotions;

    while (promotions) {
        int endSq = bitScanForward(promotions);
        promotions &= promotions - 1;
        int stSq = endSq + sqDiff;

        addPromotionsToList(quiets, stSq, endSq, false);
    }
    while (pLegal) {
        int endsq = bitScanForward(pLegal);
        pLegal &= pLegal - 1;
        quiets.add(encodeMove(endsq+sqDiff, endsq));
    }

    pLegal = (color == WHITE) ? getWPawnDoubleMoves(pawns)
                              : getBPawnDoubleMoves(pawns);
    while (pLegal) {
        int endsq = bitScanForward(pLegal);
        pLegal &= pLegal - 1;
        Move m = encodeMove(endsq+2*sqDiff, endsq);
        m = setFlags(m, MOVE_DOUBLE_PAWN);
        quiets.add(m);
    }
}

// For pawn captures, we can use a similar approach, but we must consider
// left-hand and right-hand captures separately so we can tell which
// pawn is doing the capturing.
void Board::addPawnCapturesToList(MoveList &captures, int color, uint64_t otherPieces, bool includePromotions) {
    uint64_t pawns = pieces[color][PAWNS];
    uint64_t finalRank = (color == WHITE) ? RANKS[7] : RANKS[0];
    int leftDiff = (color == WHITE) ? -7 : 9;
    int rightDiff = (color == WHITE) ? -9 : 7;

    uint64_t legal = (color == WHITE) ? getWPawnLeftCaptures(pawns)
                                      : getBPawnLeftCaptures(pawns);
    legal &= otherPieces;
    uint64_t promotions = legal & finalRank;
    legal ^= promotions;

    if (includePromotions) {
        while (promotions) {
            int endSq = bitScanForward(promotions);
            promotions &= promotions-1;

            addPromotionsToList(captures, endSq+leftDiff, endSq, true);
        }
    }
    while (legal) {
        int endsq = bitScanForward(legal);
        legal &= legal-1;
        Move m = encodeMove(endsq+leftDiff, endsq);
        m = setCapture(m, true);
        captures.add(m);
    }

    legal = (color == WHITE) ? getWPawnRightCaptures(pawns)
                             : getBPawnRightCaptures(pawns);
    legal &= otherPieces;
    promotions = legal & finalRank;
    legal ^= promotions;

    if (includePromotions) {
        while (promotions) {
            int endSq = bitScanForward(promotions);
            promotions &= promotions-1;

            addPromotionsToList(captures, endSq+rightDiff, endSq, true);
        }
    }
    while (legal) {
        int endsq = bitScanForward(legal);
        legal &= legal-1;
        Move m = encodeMove(endsq+rightDiff, endsq);
        m = setCapture(m, true);
        captures.add(m);
    }

    // If there are en passants possible...
    if (epCaptureFile != NO_EP_POSSIBLE) {
        int victimSq = epVictimSquare(color^1, epCaptureFile);
        // capturer's destination square is either the rank above (white) or
        // below (black) the victim square
        int rankDiff = (color == WHITE) ? 8 : -8;
        // The capturer's start square is either 1 to the left or right of victim
        uint64_t taker = (INDEX_TO_BIT[victimSq] << 1) & NOTA & pieces[color][PAWNS];
        if (taker) {
            Move m = encodeMove(victimSq+1, victimSq+rankDiff);
            m = setEP(m);
            captures.add(m);
        }
        else {
            taker = (INDEX_TO_BIT[victimSq] >> 1) & NOTH & pieces[color][PAWNS];
            if (taker) {
                Move m = encodeMove(victimSq-1, victimSq+rankDiff);
                m = setEP(m);
                captures.add(m);
            }
        }
    }
}

// Helper function that processes a bitboard of legal moves and adds all
// moves into a list.
void Board::addMovesToList(MoveList &moves, int stSq, uint64_t allEndSqs,
    bool isCapture, uint64_t otherPieces) {

    uint64_t intersect = (isCapture) ? otherPieces : ~getOccupancy();
    uint64_t legal = allEndSqs & intersect;
    if (isCapture) {
        while (legal) {
            int endSq = bitScanForward(legal);
            legal &= legal-1;
            Move m = encodeMove(stSq, endSq);
            m = setCapture(m, true);
            moves.add(m);
        }
    }
    else {
        while (legal) {
            int endSq = bitScanForward(legal);
            legal &= legal-1;
            Move m = encodeMove(stSq, endSq);
            moves.add(m);
        }
    }
}

void Board::addPromotionsToList(MoveList &moves, int stSq, int endSq, bool isCapture) {
    Move mk = encodeMove(stSq, endSq);
    mk = setPromotion(mk, KNIGHTS);
    Move mb = encodeMove(stSq, endSq);
    mb = setPromotion(mb, BISHOPS);
    Move mr = encodeMove(stSq, endSq);
    mr = setPromotion(mr, ROOKS);
    Move mq = encodeMove(stSq, endSq);
    mq = setPromotion(mq, QUEENS);
    if (isCapture) {
        mk = setCapture(mk, true);
        mb = setCapture(mb, true);
        mr = setCapture(mr, true);
        mq = setCapture(mq, true);
    }
    // Order promotions queen, knight, rook, bishop
    moves.add(mq);
    moves.add(mk);
    moves.add(mr);
    moves.add(mb);
}

void Board::addCastlesToList(MoveList &moves, int color) {
    // Add all possible castles
    if (color == WHITE) {
        // If castling rights still exist, squares in between king and rook are
        // empty, and player is not in check
        if ((castlingRights & WHITEKSIDE)
         && (getOccupancy() & WHITE_KSIDE_PASSTHROUGH_SQS) == 0
         && !isInCheck(WHITE)) {
            // Check for castling through check
            if (getAttackMap(BLACK, 5) == 0) {
                Move m = encodeMove(4, 6);
                m = setCastle(m, true);
                moves.add(m);
            }
        }
        if ((castlingRights & WHITEQSIDE)
         && (getOccupancy() & WHITE_QSIDE_PASSTHROUGH_SQS) == 0
         && !isInCheck(WHITE)) {
            if (getAttackMap(BLACK, 3) == 0) {
                Move m = encodeMove(4, 2);
                m = setCastle(m, true);
                moves.add(m);
            }
        }
    }
    else {
        if ((castlingRights & BLACKKSIDE)
         && (getOccupancy() & BLACK_KSIDE_PASSTHROUGH_SQS) == 0
         && !isInCheck(BLACK)) {
            if (getAttackMap(WHITE, 61) == 0) {
                Move m = encodeMove(60, 62);
                m = setCastle(m, true);
                moves.add(m);
            }
        }
        if ((castlingRights & BLACKQSIDE)
         && (getOccupancy() & BLACK_QSIDE_PASSTHROUGH_SQS) == 0
         && !isInCheck(BLACK)) {
            if (getAttackMap(WHITE, 59) == 0) {
                Move m = encodeMove(60, 58);
                m = setCastle(m, true);
                moves.add(m);
            }
        }
    }
}


//-----------------------Useful bitboard info generators:-----------------------
//------------------------------attack maps, etc.-------------------------------

// Get the attack map of all potential x-ray pieces (bishops, rooks, queens)
// after a blocker has been removed.
uint64_t Board::getXRayPieceMap(int color, int sq, int blockerColor,
    uint64_t blockerStart, uint64_t blockerEnd) {
    uint64_t occ = getOccupancy();
    occ ^= blockerStart;
    occ ^= blockerEnd;

    uint64_t bishops = pieces[color][BISHOPS];
    uint64_t rooks = pieces[color][ROOKS];
    uint64_t queens = pieces[color][QUEENS];

    uint64_t xRayMap = (getBishopSquares(sq, occ) & (bishops | queens))
                     | (getRookSquares(sq, occ) & (rooks | queens));

    return (xRayMap & ~blockerStart);
}

// Get the attack map of all potential x-ray pieces (bishops, rooks, queens)
// with no blockers removed. Used to compare with the results of the function
// above to find pins/discovered checks
/*uint64_t Board::getInitXRays(int color, int sq) {
    uint64_t bishops = pieces[color][BISHOPS];
    uint64_t rooks = pieces[color][ROOKS];
    uint64_t queens = pieces[color][QUEENS];

    return (getBishopSquares(sq) & (bishops | queens))
         | (getRookSquares(sq) & (rooks | queens));
}*/

// Given a color and a square, returns all pieces of the color that attack the
// square. Useful for checks, captures
uint64_t Board::getAttackMap(int color, int sq) {
    uint64_t occ = getOccupancy();
    uint64_t pawnCap = (color == WHITE)
                     ? getBPawnLeftCaptures(INDEX_TO_BIT[sq]) | getBPawnRightCaptures(INDEX_TO_BIT[sq])
                     : getWPawnLeftCaptures(INDEX_TO_BIT[sq]) | getWPawnRightCaptures(INDEX_TO_BIT[sq]);
    return (pawnCap & pieces[color][PAWNS])
         | (getKnightSquares(sq) & pieces[color][KNIGHTS])
         | (getBishopSquares(sq, occ) & (pieces[color][BISHOPS] | pieces[color][QUEENS]))
         | (getRookSquares(sq, occ) & (pieces[color][ROOKS] | pieces[color][QUEENS]))
         | (getKingSquares(sq) & pieces[color][KINGS]);
}

// Given the on a given square, used to get either the piece moving or the
// captured piece.
int Board::getPieceOnSquare(int color, int sq) {
    uint64_t endSingle = INDEX_TO_BIT[sq];
    for (int pieceID = 0; pieceID <= 5; pieceID++) {
        if (pieces[color][pieceID] & endSingle)
            return pieceID;
    }
    // If used for captures, the default of an empty square indicates an
    // en passant (and hopefully not an error).
    return -1;
}

// Returns true if a move puts the opponent in check
// Precondition: opposing king is not already in check (obviously)
bool Board::isCheckMove(Move m, int color) {
    int kingSq = bitScanForward(pieces[color^1][KINGS]);

    // See if move is a direct check
    uint64_t attackMap = 0;
    uint64_t occ = getOccupancy();
    switch (getPieceOnSquare(color, getStartSq(m))) {
        case PAWNS:
            attackMap = (color == WHITE) 
                ? getBPawnLeftCaptures(INDEX_TO_BIT[kingSq]) | getBPawnRightCaptures(INDEX_TO_BIT[kingSq])
                : getWPawnLeftCaptures(INDEX_TO_BIT[kingSq]) | getWPawnRightCaptures(INDEX_TO_BIT[kingSq]);
            break;
        case KNIGHTS:
            attackMap = getKnightSquares(kingSq);
            break;
        case BISHOPS:
            attackMap = getBishopSquares(kingSq, occ);
            break;
        case ROOKS:
            attackMap = getRookSquares(kingSq, occ);
            break;
        case QUEENS:
            attackMap = getQueenSquares(kingSq, occ);
            break;
        case KINGS:
            // keep attackMap 0
            break;
    }
    if (INDEX_TO_BIT[getEndSq(m)] & attackMap)
        return true;

    // See if move is a discovered check
    // Get a bitboard of all pieces that could possibly xray
    uint64_t xrayPieces = pieces[color][BISHOPS] | pieces[color][ROOKS] | pieces[color][QUEENS];

    // Get any bishops, rooks, queens attacking king after piece has moved
    uint64_t xrays = getXRayPieceMap(color, kingSq, color, INDEX_TO_BIT[getStartSq(m)], INDEX_TO_BIT[getEndSq(m)]);
    // If there is an xray piece attacking the king square after the piece has
    // moved, we have discovered check
    if (xrays & xrayPieces)
        return true;

    // If not direct or discovered check, then not a check
    return false;
}

/*
 * These two functions return all squares x-rayed by a single rook or bishop, i.e.
 * all squares between the first and second blocker.
 * Algorithm from http://chessprogramming.wikispaces.com/X-ray+Attacks+%28Bitboards%29#ModifyingOccupancy
 */
uint64_t Board::getRookXRays(int sq, uint64_t occ, uint64_t blockers) {
    uint64_t attacks = getRookSquares(sq, occ);
    blockers &= attacks;
    return attacks ^ getRookSquares(sq, occ ^ blockers);
}

uint64_t Board::getBishopXRays(int sq, uint64_t occ, uint64_t blockers) {
    uint64_t attacks = getBishopSquares(sq, occ);
    blockers &= attacks;
    return attacks ^ getBishopSquares(sq, occ ^ blockers);
}

/*
 * This function returns all pinned pieces for a given side.
 * Algorithm from http://chessprogramming.wikispaces.com/Checks+and+Pinned+Pieces+%28Bitboards%29
 */
uint64_t Board::getPinnedMap(int color) {
    uint64_t pinned = 0;
    uint64_t blockers = allPieces[color];
    int kingSq = bitScanForward(pieces[color][KINGS]);

    uint64_t pinners = getRookXRays(kingSq, getOccupancy(), blockers)
        & (pieces[color^1][ROOKS] | pieces[color^1][QUEENS]);
    while (pinners) {
        int sq  = bitScanForward(pinners);
        pinned |= inBetweenSqs[sq][kingSq] & blockers;
        pinners &= pinners - 1;
    }

    pinners = getBishopXRays(kingSq, getOccupancy(), blockers)
        & (pieces[color^1][BISHOPS] | pieces[color^1][QUEENS]);
    while (pinners) {
        int sq  = bitScanForward(pinners);
        pinned |= inBetweenSqs[sq][kingSq] & blockers;
        pinners &= pinners - 1;
    }

    return pinned;
}

//----------------------King: check, checkmate, stalemate-----------------------
bool Board::isInCheck(int color) {
    int sq = bitScanForward(pieces[color][KINGS]);

    return getAttackMap(color^1, sq);
}

// Checks for mate: white is in check and has no legal moves
bool Board::isWInMate() {
    if (!isInCheck(WHITE))
        return false;

    MoveList moves = getAllLegalMoves(WHITE);
    return (moves.size() == 0);
}

// Checks for mate: black is in check and has no legal moves
bool Board::isBInMate() {
    if (!isInCheck(BLACK))
        return false;

    MoveList moves = getAllLegalMoves(BLACK);
    return (moves.size() == 0);
}

// Checks for stalemate: side to move is not in check, but has no legal moves
bool Board::isStalemate(int sideToMove) {
    if (isInCheck(sideToMove))
        return false;

    MoveList moves = getAllLegalMoves(sideToMove);
    return (moves.size() == 0);
}

bool Board::isDraw() {
    if (fiftyMoveCounter >= 100) return true;

    // If the highest bit is set, this is a flag telling us the two-fold repetition
    // counter was reset too recently
    if ((twoFoldSqs >> 63) == 0) {
        uint8_t *pTwoFoldSqs = (uint8_t*) (&twoFoldSqs);
        
        /* Check for a loop in moves. For example:
         * =====most recent=====
         * 0 b2 (white end sq)
         * 1 a1 (white start sq)
         * 2 a8 (black end sq)
         * 3 b7 (black start sq)
         * 4 a1 (white end sq)
         * 5 b2 (white start sq)
         * 6 b7 (black end sq)
         * 7 a8 (black start sq)
         * =====least recent=====
         */
        if ((pTwoFoldSqs[0] == pTwoFoldSqs[5])
         && (pTwoFoldSqs[1] == pTwoFoldSqs[4])
         && (pTwoFoldSqs[2] == pTwoFoldSqs[7])
         && (pTwoFoldSqs[3] == pTwoFoldSqs[6])) {
            return true;
        }
    }
    
    return false;
}

//------------------------------------------------------------------------------
//------------------------Evaluation and Move Ordering--------------------------
//------------------------------------------------------------------------------
/*
 * Evaluates the current board position in hundredths of pawns. White is
 * positive and black is negative in traditional negamax format.
 */
int Board::evaluate() {
    return evaluateMaterial() + evaluatePositional();
}

// Helper functions for lazy evaluation
int Board::evaluateMaterial() {
    // Tempo bonus
    int value = (playerToMove == WHITE) ? TEMPO_VALUE : -TEMPO_VALUE;

    // material
    int whiteMaterial = getMaterial(WHITE);
    int blackMaterial = getMaterial(BLACK);
    
    // compute endgame factor which is between 0 and EG_FACTOR_RES, inclusive
    int egFactor = EG_FACTOR_RES - (whiteMaterial + blackMaterial - START_VALUE / 2) * EG_FACTOR_RES / START_VALUE;
    egFactor = std::max(0, std::min(EG_FACTOR_RES, egFactor));
    
    value += whiteMaterial + (PAWN_VALUE_EG - PAWN_VALUE) * count(pieces[WHITE][PAWNS]) * egFactor / EG_FACTOR_RES;
    value -= blackMaterial + (PAWN_VALUE_EG - PAWN_VALUE) * count(pieces[BLACK][PAWNS]) * egFactor / EG_FACTOR_RES;
    
    // bishop pair bonus
    if ((pieces[WHITE][BISHOPS] & LIGHT) && (pieces[WHITE][BISHOPS] & DARK))
        value += BISHOP_PAIR_VALUE;
    if ((pieces[BLACK][BISHOPS] & LIGHT) && (pieces[BLACK][BISHOPS] & DARK))
        value -= BISHOP_PAIR_VALUE;
    
    // piece square tables
    int midgamePSTVal = 0;
    int endgamePSTVal = 0;
    // White pieces
    for (int pieceID = 0; pieceID < 6; pieceID++) {
        uint64_t bitboard = pieces[0][pieceID];
        // Invert the board for white side
        bitboard = flipAcrossRanks(bitboard);
        while (bitboard) {
            int i = bitScanForward(bitboard);
            bitboard &= bitboard - 1;
            midgamePSTVal += midgamePieceValues[pieceID][i];
            endgamePSTVal += endgamePieceValues[pieceID][i];
        }
    }
    // Black pieces
    for (int pieceID = 0; pieceID < 6; pieceID++)  {
        uint64_t bitboard = pieces[1][pieceID];
        while (bitboard) {
            int i = bitScanForward(bitboard);
            bitboard &= bitboard - 1;
            midgamePSTVal -= midgamePieceValues[pieceID][i];
            endgamePSTVal -= endgamePieceValues[pieceID][i];
        }
    }
    // Adjust values according to material left on board
    value += midgamePSTVal * (EG_FACTOR_RES - egFactor) / EG_FACTOR_RES;
    value += endgamePSTVal * egFactor / EG_FACTOR_RES;
    
    return value;
}

int Board::evaluatePositional() {
    int value = 0;
    
    // material
    int whiteMaterial = getMaterial(WHITE);
    int blackMaterial = getMaterial(BLACK);
    
    // compute endgame factor which is between 0 and EG_FACTOR_RES, inclusive
    int egFactor = EG_FACTOR_RES - (whiteMaterial + blackMaterial - START_VALUE / 2) * EG_FACTOR_RES / START_VALUE;
    egFactor = std::max(0, std::min(EG_FACTOR_RES, egFactor));
    
    // Consider attacked squares near king
    uint64_t wksq = getKingSquares(bitScanForward(pieces[WHITE][KINGS]));
    uint64_t bksq = getKingSquares(bitScanForward(pieces[BLACK][KINGS]));
    
    // Pawn shield bonus (files ABC, FGH)
    value += (25 * egFactor / EG_FACTOR_RES) * count(wksq & pieces[WHITE][PAWNS] & 0xe7e7e7e7e7e7e7e7);
    value -= (25 * egFactor / EG_FACTOR_RES) * count(bksq & pieces[BLACK][PAWNS] & 0xe7e7e7e7e7e7e7e7);
    
    // Scores based on mobility and basic king safety (which is turned off in
    // the endgame)
    value += getPseudoMobility(WHITE, bksq, egFactor);
    value -= getPseudoMobility(BLACK, wksq, egFactor);

    // Pawn structure
    // Passed pawns
    uint64_t notwp = pieces[WHITE][PAWNS];
    uint64_t notbp = pieces[BLACK][PAWNS];
    // These act as blockers for the flood fill: if opposing pawns are on the
    // same or an adjacent rank, your pawn is not passed.
    notwp |= ((notwp >> 1) & NOTH) | ((notwp << 1) & NOTA);
    notbp |= ((notbp >> 1) & NOTH) | ((notbp << 1) & NOTA);
    notwp = ~notwp;
    notbp = ~notbp;
    uint64_t tempwp = pieces[WHITE][PAWNS];
    uint64_t tempbp = pieces[BLACK][PAWNS];
    // Flood fill to simulate pushing the pawn to the 7th (or 2nd) rank
    for(int i = 0; i < 6; i++) {
        tempwp |= (tempwp << 8) & notbp;
        tempbp |= (tempbp >> 8) & notwp;
    }
    // Pawns that made it without being blocked are passed
    value += (10 + 50 * egFactor / EG_FACTOR_RES) * count(tempwp & RANKS[7]);
    value -= (10 + 50 * egFactor / EG_FACTOR_RES) * count(tempbp & RANKS[0]);

    int wPawnCtByFile[8];
    int bPawnCtByFile[8];
    for (int i = 0; i < 8; i++) {
        wPawnCtByFile[i] = count(pieces[WHITE][PAWNS] & FILES[i]);
        bPawnCtByFile[i] = count(pieces[BLACK][PAWNS] & FILES[i]);
    }

    // Doubled pawns
    // 0 pawns on file: 0 cp
    // 1 pawn on file: 0 cp (each pawn worth 100 cp)
    // 2 pawns on file: -24 cp (each pawn worth 88 cp)
    // 3 pawns on file: -48 cp (each pawn worth 84 cp)
    // 4 pawns on file: -144 cp (each pawn worth 64 cp)
    for (int i = 0; i < 8; i++) {
        value -= 24 * (wPawnCtByFile[i] - 1) * (wPawnCtByFile[i] / 2);
        value += 24 * (bPawnCtByFile[i] - 1) * (bPawnCtByFile[i] / 2);
    }

    // Isolated pawns
    uint64_t wp = 0, bp = 0;
    for (int i = 0; i < 8; i++) {
        wp |= (bool) (wPawnCtByFile[i]);
        bp |= (bool) (bPawnCtByFile[i]);
        wp <<= 1;
        bp <<= 1;
    }
    // If there are pawns on either adjacent file, we remove this pawn
    wp &= ~((wp >> 1) | (wp << 1));
    bp &= ~((bp >> 1) | (bp << 1));
    value -= 20 * count(wp);
    value += 20 * count(bp);

    return value;
}

// Scores the board for a player based on estimates of mobility
// This function also provides a bonus for having mobile pieces near the
// opponent's king
int Board::getPseudoMobility(int color, uint64_t oppKingSqs, int egFactor) {
    int result = 0;
    int kingSafety = 0;
    uint64_t knights = pieces[color][KNIGHTS];
    uint64_t bishops = pieces[color][BISHOPS];
    uint64_t rooks = pieces[color][ROOKS];
    uint64_t queens = pieces[color][QUEENS];
    uint64_t pieces = allPieces[color];
    const int KING_THREAT_MOBILITY = 15;

    while (knights != 0) {
        int single = bitScanForward(knights);
        knights &= knights-1;

        uint64_t legal = getKnightSquares(single) & ~pieces;

        result += knightMobility[count(legal)];
        kingSafety += KING_THREAT_MOBILITY * count(legal & oppKingSqs);
    }

    uint64_t occ = getOccupancy();
    while (bishops != 0) {
        int single = bitScanForward(bishops);
        bishops &= bishops-1;

        uint64_t legal = getBishopSquares(single, occ) & ~pieces;

        result += bishopMobility[count(legal)];
        kingSafety += KING_THREAT_MOBILITY * count(legal & oppKingSqs);
    }

    while (rooks != 0) {
        int single = bitScanForward(rooks);
        rooks &= rooks-1;

        uint64_t legal = getRookSquares(single, occ) & ~pieces;

        result += rookMobility[count(legal)];
        kingSafety += KING_THREAT_MOBILITY * count(legal & oppKingSqs);
    }

    while (queens != 0) {
        int single = bitScanForward(queens);
        queens &= queens-1;

        uint64_t legal = getQueenSquares(single, occ) & ~pieces;

        result += queenMobility[count(legal)];
        kingSafety += KING_THREAT_MOBILITY * count(legal & oppKingSqs);
    }

    return result + kingSafety * (EG_FACTOR_RES - egFactor) / EG_FACTOR_RES;
}

// Gets the endgame factor, which adjusts the evaluation based on how much
// material is left on the board.
int Board::getEGFactor() {
    int whiteMaterial = getMaterial(WHITE);
    int blackMaterial = getMaterial(BLACK);
    int egFactor = EG_FACTOR_RES - (whiteMaterial + blackMaterial - START_VALUE / 2) * EG_FACTOR_RES / START_VALUE;
    return std::max(0, std::min(EG_FACTOR_RES, egFactor));
}

int Board::getMaterial(int color) {
    return PAWN_VALUE   * count(pieces[color][PAWNS])
         + KNIGHT_VALUE * count(pieces[color][KNIGHTS])
         + BISHOP_VALUE * count(pieces[color][BISHOPS])
         + ROOK_VALUE   * count(pieces[color][ROOKS])
         + QUEEN_VALUE  * count(pieces[color][QUEENS]);
}

uint64_t Board::getNonPawnMaterial(int color) {
    return pieces[color][KNIGHTS] | pieces[color][BISHOPS]
         | pieces[color][ROOKS]   | pieces[color][QUEENS];
}

// Given a bitboard of attackers, finds the least valuable attacker of color and
// returns a single occupancy bitboard of that piece
uint64_t Board::getLeastValuableAttacker(uint64_t attackers, int color, int &piece) {
    for (piece = 0; piece < 5; piece++) {
        uint64_t single = attackers & pieces[color][piece];
        if (single)
            return single & -single;
    }

    piece = KINGS;
    return attackers & pieces[color][KINGS];
}

// Static exchange evaluation algorithm from
// https://chessprogramming.wikispaces.com/SEE+-+The+Swap+Algorithm
int Board::getSEE(int color, int sq) {
    int gain[32], d = 0, piece = 0;
    uint64_t attackers = getAttackMap(WHITE, sq) | getAttackMap(BLACK, sq);
    // used attackers that may act as blockers for x-ray pieces
    uint64_t used = 0;
    uint64_t single = getLeastValuableAttacker(attackers, color, piece);
    // Get value of piece initially being captured, or 0 if the square is
    // empty
    gain[d] = valueOfPiece(getPieceOnSquare(color^1, sq));

    do {
        d++; // next depth
        color ^= 1;
        gain[d]  = valueOfPiece(piece) - gain[d-1];
        if (-gain[d-1] < 0 && gain[d] < 0) // pruning for stand pat
            break;
        attackers ^= single; // remove used attacker
        used |= single;
        attackers |= getXRayPieceMap(WHITE, sq, color, used, 0) | getXRayPieceMap(BLACK, sq, color, used, 0);
        single = getLeastValuableAttacker(attackers, color, piece);
    } while (single);

    while (--d)
        gain[d-1]= -((-gain[d-1] > gain[d]) ? -gain[d-1] : gain[d]);

    return gain[0];
}

int Board::valueOfPiece(int piece) {
    switch(piece) {
        case -1:
            return 0;
        case PAWNS:
            return PAWN_VALUE;
        case KNIGHTS:
            return KNIGHT_VALUE;
        case BISHOPS:
            return BISHOP_VALUE;
        case ROOKS:
            return ROOK_VALUE;
        case QUEENS:
            return QUEEN_VALUE;
        case KINGS:
            return MATE_SCORE;
    }

    return -1;
}

// Calculates a score for Most Valuable Victim / Least Valuable Attacker
// capture ordering.
int Board::getMVVLVAScore(int color, Move m) {
    int endSq = getEndSq(m);
    int attacker = getPieceOnSquare(color, getStartSq(m));
    int victim = getPieceOnSquare(color^1, endSq);
    if (attacker == KINGS)
        attacker = -1;

    return (victim * 8) + (4 - attacker);
}

// Returns a score from the initial capture
// This helps reduce the number of times SEE must be used in quiescence search,
// since if we have a losing trade after capture-recapture our opponent could
// likely stand pat, or we could've captured with a better piece
int Board::getExchangeScore(int color, Move m) {
    int endSq = getEndSq(m);
    int attacker = getPieceOnSquare(color, getStartSq(m));
    int victim = getPieceOnSquare(color^1, endSq);
    return victim - attacker;
}


//------------------------------------------------------------------------------
//-----------------------------MOVE GENERATION----------------------------------
//------------------------------------------------------------------------------
inline uint64_t Board::getWPawnSingleMoves(uint64_t pawns) {
    return (pawns << 8) & ~getOccupancy();
}

inline uint64_t Board::getBPawnSingleMoves(uint64_t pawns) {
    return (pawns >> 8) & ~getOccupancy();
}

uint64_t Board::getWPawnDoubleMoves(uint64_t pawns) {
    uint64_t open = ~getOccupancy();
    uint64_t temp = (pawns << 8) & open;
    pawns = (temp << 8) & open & RANKS[3];
    return pawns;
}

uint64_t Board::getBPawnDoubleMoves(uint64_t pawns) {
    uint64_t open = ~getOccupancy();
    uint64_t temp = (pawns >> 8) & open;
    pawns = (temp >> 8) & open & RANKS[4];
    return pawns;
}

inline uint64_t Board::getWPawnLeftCaptures(uint64_t pawns) {
    return (pawns << 7) & NOTH;
}

inline uint64_t Board::getBPawnLeftCaptures(uint64_t pawns) {
    return (pawns >> 9) & NOTH;
}

inline uint64_t Board::getWPawnRightCaptures(uint64_t pawns) {
    return (pawns << 9) & NOTA;
}

inline uint64_t Board::getBPawnRightCaptures(uint64_t pawns) {
    return (pawns >> 7) & NOTA;
}

inline uint64_t Board::getKnightSquares(int single) {
    return KNIGHTMOVES[single];
}

uint64_t Board::getBishopSquares(int single, uint64_t occ) {
    uint64_t *attTableLoc = magicBishops[single].table;
    occ &= magicBishops[single].mask;
    occ *= magicBishops[single].magic;
    occ >>= magicBishops[single].shift;
    return attTableLoc[occ];
}

uint64_t Board::getRookSquares(int single, uint64_t occ) {
    uint64_t *attTableLoc = magicRooks[single].table;
    occ &= magicRooks[single].mask;
    occ *= magicRooks[single].magic;
    occ >>= magicRooks[single].shift;
    return attTableLoc[occ];
}

uint64_t Board::getQueenSquares(int single, uint64_t occ) {
    return getBishopSquares(single, occ) | getRookSquares(single, occ);
}

inline uint64_t Board::getKingSquares(int single) {
    return KINGMOVES[single];
}

inline uint64_t Board::getOccupancy() {
    return allPieces[WHITE] | allPieces[BLACK];
}


//------------------------------------------------------------------------------
//---------------------------PUBLIC GETTER METHODS------------------------------
//------------------------------------------------------------------------------
bool Board::getWhiteCanKCastle() {
    return castlingRights & WHITEKSIDE;
}

bool Board::getBlackCanKCastle() {
    return castlingRights & BLACKKSIDE;
}

bool Board::getWhiteCanQCastle() {
    return castlingRights & WHITEQSIDE;
}

bool Board::getBlackCanQCastle() {
    return castlingRights & BLACKQSIDE;
}

uint16_t Board::getEPCaptureFile() {
    return epCaptureFile;
}

uint8_t Board::getFiftyMoveCounter() {
    return fiftyMoveCounter;
}

uint16_t Board::getMoveNumber() {
    return moveNumber;
}

int Board::getPlayerToMove() {
    return playerToMove;
}

uint64_t Board::getAllPieces(int color) {
    return allPieces[color];
}

int *Board::getMailbox() {
    int *result = new int[64];
    for (int i = 0; i < 64; i++) {
        result[i] = -1;
    }
    for (int i = 0; i < 6; i++) {
        uint64_t bitboard = pieces[0][i];
        while (bitboard) {
            result[bitScanForward(bitboard)] = i;
            bitboard &= bitboard - 1;
        }
    }
    for (int i = 0; i < 6; i++) {
        uint64_t bitboard = pieces[1][i];
        while (bitboard) {
            result[bitScanForward(bitboard)] = 6 + i;
            bitboard &= bitboard - 1;
        }
    }
    return result;
}

uint64_t Board::getZobristKey() {
    return zobristKey;
}

void Board::initZobristKey(int *mailbox) {
    zobristKey = 0;
    for (int i = 0; i < 64; i++) {
        if (mailbox[i] != -1) {
            zobristKey ^= zobristTable[mailbox[i] * 64 + i];
        }
    }
    if (playerToMove == BLACK)
        zobristKey ^= zobristTable[768];
    zobristKey ^= zobristTable[769 + castlingRights];
    zobristKey ^= zobristTable[785 + epCaptureFile];
}

// Dumb7Fill
uint64_t fillRayRight(uint64_t rayPieces, uint64_t empty, int shift) {
    uint64_t flood = rayPieces;
    // To prevent overflow across the sides of the board on east/west fills
    uint64_t borderMask = 0xFFFFFFFFFFFFFFFF;
    if (shift == 1 || shift == 9)
        borderMask = NOTH;
    else if (shift == 7)
        borderMask = NOTA;
    empty &= borderMask;
    flood |= rayPieces = (rayPieces >> shift) & empty;
    flood |= rayPieces = (rayPieces >> shift) & empty;
    flood |= rayPieces = (rayPieces >> shift) & empty;
    flood |= rayPieces = (rayPieces >> shift) & empty;
    flood |= rayPieces = (rayPieces >> shift) & empty;
    flood |=         (rayPieces >> shift) & empty;
    return           (flood >> shift) & borderMask;
}

uint64_t fillRayLeft(uint64_t rayPieces, uint64_t empty, int shift) {
    uint64_t flood = rayPieces;
    // To prevent overflow across the sides of the board on east/west fills
    uint64_t borderMask = 0xFFFFFFFFFFFFFFFF;
    if (shift == 1 || shift == 9)
        borderMask = NOTA;
    else if (shift == 7)
        borderMask = NOTH;
    empty &= borderMask;
    flood |= rayPieces = (rayPieces << shift) & empty;
    flood |= rayPieces = (rayPieces << shift) & empty;
    flood |= rayPieces = (rayPieces << shift) & empty;
    flood |= rayPieces = (rayPieces << shift) & empty;
    flood |= rayPieces = (rayPieces << shift) & empty;
    flood |=         (rayPieces << shift) & empty;
    return           (flood << shift) & borderMask;
}


//------------------------------------------------------------------------------
//-----------------------------MAGIC BITBOARDS----------------------------------
//------------------------------------------------------------------------------
// This code is adapted from Tord Romstad's approach to finding magics,
// available online at https://chessprogramming.wikispaces.com/Looking+for+Magics

// Maps an index from 0 from 2^nBits - 1 into one of the
// 2^nBits possible masks
uint64_t indexToMask64(int index, int nBits, uint64_t mask) {
    uint64_t result = 0;
    // For each bit in the mask
    for (int i = 0; i < nBits; i++) {
        int j = bitScanForward(mask);
        mask &= mask - 1;
        // If this bit should be set...
        if (index & INDEX_TO_BIT[i])
            // Set it at the same position in the result
            result |= INDEX_TO_BIT[j];
    }
    return result;
}

// Gets rook attacks using Dumb7Fill methods
uint64_t ratt(int sq, uint64_t block) {
    return fillRayRight(INDEX_TO_BIT[sq], ~block, NORTH_SOUTH_FILL) // south
         | fillRayLeft(INDEX_TO_BIT[sq], ~block, NORTH_SOUTH_FILL)  // north
         | fillRayLeft(INDEX_TO_BIT[sq], ~block, EAST_WEST_FILL)    // east
         | fillRayRight(INDEX_TO_BIT[sq], ~block, EAST_WEST_FILL);  // west
}

// Gets bishop attacks using Dumb7Fill methods
uint64_t batt(int sq, uint64_t block) {
    return fillRayLeft(INDEX_TO_BIT[sq], ~block, NE_SW_FILL)   // northeast
         | fillRayLeft(INDEX_TO_BIT[sq], ~block, NW_SE_FILL)   // northwest
         | fillRayRight(INDEX_TO_BIT[sq], ~block, NE_SW_FILL)  // southwest
         | fillRayRight(INDEX_TO_BIT[sq], ~block, NW_SE_FILL); // southeast
}

// Maps a mask using a candidate magic into an index nBits long
int magicMap(uint64_t masked, uint64_t magic, int nBits) {
    return (int) ((masked * magic) >> (64 - nBits));
}

/**
 * @brief Finds a magic number for the given square using trial and error.
 * @param sq The square to find the magic for.
 * @param iBits The length of the desired index, in bits
 * @param isBishop True for bishop magics, false for rook magics
 * @param magicRNG A random number generator to get magic candidates
 */
uint64_t findMagic(int sq, int iBits, bool isBishop) {
    uint64_t mask, maskedBits[4096], attSet[4096], used[4096], magic;
    bool failed;

    mask = isBishop ? BISHOP_MASK[sq] : ROOK_MASK[sq];
    int nBits = count(mask);
    // For each possible masked occupancy, get the attack set corresponding to
    // that square and occupancy
    for (int i = 0; i < (1 << nBits); i++) {
        maskedBits[i] = indexToMask64(i, nBits, mask);
        attSet[i] = isBishop ? batt(sq, maskedBits[i]) : ratt(sq, maskedBits[i]);
    }
    // Try 100 mill iterations before giving up
    for (int k = 0; k < 100000000; k++) {
        // Get a random magic candidate
        // We make this random 64-bit integer sparse by &-ing 3 random numbers
        // Sparse numbers are beneficial to keep the multiplied bits from
        // bleeding together and becoming garbage
        magic = magicRNG() & magicRNG() & magicRNG();
        // We want a large number of high bits to get a higher success rate,
        // since mask * magic is shifted by 64 - n bits, leaving n bits at the
        // end. Thus, anything but the top 12 bits (for rooks in the corners) or
        // less (for bishops) is garbage. Having a large number of high bits
        // when multiplying by the full mask gives a better spread of values
        // with different partial masks.
        if (count((mask * magic) & 0xFFF0000000000000ULL) < 10)
            continue;

        // Clear the used table
        for (int i = 0; i < 4096; i++)
            used[i] = 0;
        // Calculate the packed bits for every possible mask using this magic
        // and see if any fail
        failed = false;
        for (int i = 0; !failed && i < (1 << nBits); i++) {
            int mappedIndex = magicMap(maskedBits[i], magic, iBits);
            // No collision, mark the index as used for the given attack set
            if (!used[mappedIndex])
                used[mappedIndex] = attSet[i];
            // Otherwise, check for a constructive collsion, where a different
            // occupancy has the same attack set.
            // If the collision is not constructive, then we failed.
            else if (used[mappedIndex] != attSet[i])
                failed = true;
        }
        // If there were no collisions in all 2^nBits mappings, we have found
        // a valid magic
        if (!failed)
            return magic;
    }

    // Otherwise we failed :(
    // (this should never happen)
    return 0;
}