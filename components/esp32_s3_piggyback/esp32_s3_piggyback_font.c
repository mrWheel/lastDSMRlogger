#include <stdint.h>
#include <stddef.h>

/* Compact 5x7 uppercase font. Each byte is one row, bit 4 is the left pixel. */
uint8_t tft_ec11_font_row(unsigned char c, uint8_t row)
{
    //-- Fold accented Latin-1 letters (as decoded from 2-byte UTF-8 by the
    //-- caller) onto their unaccented base letter so the existing A-Z glyphs
    //-- can be reused instead of drawing separate accent glyphs.
    switch (c)
    {
        case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5:
            c = 'A';
            break;
        case 0xC7: case 0xE7:
            c = 'C';
            break;
        case 0xC8: case 0xC9: case 0xCA: case 0xCB:
        case 0xE8: case 0xE9: case 0xEA: case 0xEB:
            c = 'E';
            break;
        case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xEC: case 0xED: case 0xEE: case 0xEF:
            c = 'I';
            break;
        case 0xD1: case 0xF1:
            c = 'N';
            break;
        case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8:
        case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8:
            c = 'O';
            break;
        case 0xD9: case 0xDA: case 0xDB: case 0xDC:
        case 0xF9: case 0xFA: case 0xFB: case 0xFC:
            c = 'U';
            break;
        case 0xDD: case 0xFF:
            c = 'Y';
            break;
        default:
            break;
    }

    static const uint8_t digits[10][7] = {
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
        {14,17,17,15,1,1,14}
    };
    static const uint8_t letters[26][7] = {
        {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
        {7,2,2,2,2,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4},{31,1,2,4,8,16,31}
    };
    if (row > 6) return 0;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= '0' && c <= '9') return digits[c-'0'][row];
    if (c >= 'A' && c <= 'Z') return letters[c-'A'][row];
    if (c == '-') { static const uint8_t p[7]={0,0,0,31,0,0,0}; return p[row]; }
    if (c == '+') { static const uint8_t p[7]={0,4,4,31,4,4,0}; return p[row]; }
    if (c == ':') return (row==2 || row==5) ? 4 : 0;
    if (c == '.') return row==6 ? 4 : 0;
    if (c == '/') { static const uint8_t p[7]={1,2,2,4,8,8,16}; return p[row]; }
    if (c == ' ') return 0;

    //-- Additional punctuation / special characters.
    static const struct
    {
        unsigned char character;
        uint8_t rows[7];
    } punctuation[] = {
        {'!',  {4,4,4,4,4,0,4}},
        {'"',  {10,10,0,0,0,0,0}},
        {'#',  {10,10,31,10,31,10,10}},
        {'$',  {4,15,20,14,5,30,4}},
        {'%',  {25,26,2,4,8,19,19}},
        {'&',  {12,18,20,8,21,18,13}},
        {'\'', {4,4,0,0,0,0,0}},
        {'(',  {2,4,8,8,8,4,2}},
        {')',  {8,4,2,2,2,4,8}},
        {'*',  {0,10,4,31,4,10,0}},
        {',',  {0,0,0,0,0,4,8}},
        {';',  {0,0,4,0,0,4,8}},
        {'<',  {2,4,8,16,8,4,2}},
        {'=',  {0,0,31,0,31,0,0}},
        {'>',  {8,4,2,1,2,4,8}},
        {'?',  {14,17,1,2,4,0,4}},
        {'@',  {14,17,23,21,22,16,14}},
        {'[',  {12,8,8,8,8,8,12}},
        {'\\', {16,8,4,4,2,2,1}},
        {']',  {6,2,2,2,2,2,6}},
        {'^',  {4,10,17,0,0,0,0}},
        {'_',  {0,0,0,0,0,0,31}},
        {'`',  {16,8,0,0,0,0,0}},
        {'{',  {6,8,8,16,8,8,6}},
        {'|',  {4,4,4,4,4,4,4}},
        {'}',  {12,2,2,1,2,2,12}},
        {'~',  {0,0,6,24,0,0,0}}
    };
    for (size_t i = 0; i < sizeof(punctuation) / sizeof(punctuation[0]); i++)
    {
        if (punctuation[i].character == c) return punctuation[i].rows[row];
    }

    return (row==0 || row==6) ? 31 : ((row==3) ? 21 : 17);
}
