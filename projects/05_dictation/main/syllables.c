/*
 * MX-2 vocabulary. See syllables.h for why the two languages get different tables.
 *
 * The Mandarin table is generated rather than written out: every initial crossed with
 * every final, then handed to MultiNet, which rejects what it cannot tokenise and says
 * so ("command not loadable"). That is cheaper than curating a list by hand and it
 * self-corrects against whatever the engine's own G2P actually accepts. The cross
 * product overshoots the real inventory - many combinations are not Mandarin syllables
 * at all - so it is capped and the engine has the last word.
 */

#include "syllables.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

/* MultiNet's documented ceiling is 300. Stay under it. */
#define MAX_SYLLABLES 280
#define SYL_MAX_LEN   8

#if CONFIG_DICT_LANG_CN

/*
 * The Mandarin inventory, written out per initial rather than generated.
 *
 * The first attempt crossed 22 initials with 13 finals and let MultiNet reject what
 * it did not like. Pinyin is not a cross product - "be", "fe", "do", "no", "go" are
 * not syllables - so 355 were rejected, and esp-sr then died with a LoadProhibited
 * null deref inside "Build fst from commands". The engine cannot be used as the
 * filter; it has to be handed a clean list.
 *
 * 215 syllables, restricted to the 13 common finals, against MultiNet's 300 ceiling.
 * That is most of what toneless Mandarin needs, which is the whole reason this
 * language is the B2 candidate and English is only a control.
 */
static const word_def_t k_syllables[] = {
    {"a"}, {"o"}, {"e"}, {"ai"}, {"ei"}, {"ao"}, {"ou"}, {"an"}, {"en"},
    {"ang"}, {"eng"}, {"ba"}, {"bo"}, {"bi"}, {"bu"}, {"bai"}, {"bei"}, {"bao"},
    {"ban"}, {"ben"}, {"bang"}, {"beng"}, {"pa"}, {"po"}, {"pi"}, {"pu"}, {"pai"},
    {"pei"}, {"pao"}, {"pou"}, {"pan"}, {"pen"}, {"pang"}, {"peng"}, {"ma"}, {"mo"},
    {"me"}, {"mi"}, {"mu"}, {"mai"}, {"mei"}, {"mao"}, {"mou"}, {"man"}, {"men"},
    {"mang"}, {"meng"}, {"fa"}, {"fo"}, {"fu"}, {"fei"}, {"fou"}, {"fan"}, {"fen"},
    {"fang"}, {"feng"}, {"da"}, {"de"}, {"di"}, {"du"}, {"dai"}, {"dei"}, {"dao"},
    {"dou"}, {"dan"}, {"den"}, {"dang"}, {"deng"}, {"ta"}, {"te"}, {"ti"}, {"tu"},
    {"tai"}, {"tao"}, {"tou"}, {"tan"}, {"tang"}, {"teng"}, {"na"}, {"ne"}, {"ni"},
    {"nu"}, {"nai"}, {"nei"}, {"nao"}, {"nou"}, {"nan"}, {"nen"}, {"nang"}, {"neng"},
    {"la"}, {"le"}, {"li"}, {"lu"}, {"lai"}, {"lei"}, {"lao"}, {"lou"}, {"lan"},
    {"lang"}, {"leng"}, {"ga"}, {"ge"}, {"gu"}, {"gai"}, {"gei"}, {"gao"}, {"gou"},
    {"gan"}, {"gen"}, {"gang"}, {"geng"}, {"ka"}, {"ke"}, {"ku"}, {"kai"}, {"kei"},
    {"kao"}, {"kou"}, {"kan"}, {"ken"}, {"kang"}, {"keng"}, {"ha"}, {"he"}, {"hu"},
    {"hai"}, {"hei"}, {"hao"}, {"hou"}, {"han"}, {"hen"}, {"hang"}, {"heng"}, {"ji"},
    {"qi"}, {"xi"}, {"zha"}, {"zhe"}, {"zhi"}, {"zhu"}, {"zhai"}, {"zhei"}, {"zhao"},
    {"zhou"}, {"zhan"}, {"zhen"}, {"zhang"}, {"zheng"}, {"cha"}, {"che"}, {"chi"}, {"chu"},
    {"chai"}, {"chao"}, {"chou"}, {"chan"}, {"chen"}, {"chang"}, {"cheng"}, {"sha"}, {"she"},
    {"shi"}, {"shu"}, {"shai"}, {"shei"}, {"shao"}, {"shou"}, {"shan"}, {"shen"}, {"shang"},
    {"sheng"}, {"re"}, {"ri"}, {"ru"}, {"rao"}, {"rou"}, {"ran"}, {"ren"}, {"rang"},
    {"reng"}, {"za"}, {"ze"}, {"zi"}, {"zu"}, {"zai"}, {"zei"}, {"zao"}, {"zou"},
    {"zan"}, {"zen"}, {"zang"}, {"zeng"}, {"ca"}, {"ce"}, {"ci"}, {"cu"}, {"cai"},
    {"cao"}, {"cou"}, {"can"}, {"cen"}, {"cang"}, {"ceng"}, {"sa"}, {"se"}, {"si"},
    {"su"}, {"sai"}, {"sao"}, {"sou"}, {"san"}, {"sen"}, {"sang"}, {"seng"},
};

const word_def_t *syllables_table(size_t *count)
{
    *count = sizeof(k_syllables) / sizeof(k_syllables[0]);
    return k_syllables;
}

const char *syllables_kind(void) { return "mandarin syllables (215, curated)"; }

#else

/*
 * English control. Common monosyllables, not syllables: the English inventory cannot
 * fit in 300 commands, so this table cannot test coverage. It tests the only thing
 * that decides B2 - whether MultiNet fires repeatedly through continuous speech or
 * detects once and times out. Real words are used because G2P handles them reliably,
 * where invented syllable spellings are ambiguous ("ba" could be /ba/ or /bei/).
 */
static const word_def_t k_words[] = {
    {"a"},     {"all"},   {"and"},   {"are"},   {"as"},    {"at"},    {"back"},  {"be"},    {"been"},
    {"big"},   {"but"},   {"by"},    {"call"},  {"came"},  {"can"},   {"come"},  {"could"}, {"day"},
    {"did"},   {"do"},    {"does"},  {"down"},  {"each"},  {"end"},   {"far"},   {"few"},   {"find"},
    {"first"}, {"for"},   {"found"}, {"from"},  {"get"},   {"give"},  {"go"},    {"good"},  {"got"},
    {"great"}, {"had"},   {"hand"},  {"has"},   {"have"},  {"he"},    {"head"},  {"help"},  {"her"},
    {"here"},  {"high"},  {"him"},   {"his"},   {"home"},  {"how"},   {"if"},    {"in"},    {"into"},
    {"is"},    {"it"},    {"its"},   {"just"},  {"keep"},  {"kind"},  {"know"},  {"land"},  {"large"},
    {"last"},  {"left"},  {"let"},   {"life"},  {"like"},  {"line"},  {"long"},  {"look"},  {"made"},
    {"make"},  {"man"},   {"many"},  {"may"},   {"me"},    {"men"},   {"might"}, {"more"},  {"most"},
    {"move"},  {"much"},  {"must"},  {"my"},    {"name"},  {"near"},  {"need"},  {"new"},   {"next"},
    {"no"},    {"not"},   {"now"},   {"number"},{"of"},    {"off"},   {"old"},   {"on"},    {"one"},
    {"only"},  {"open"},  {"or"},    {"other"}, {"our"},   {"out"},   {"over"},  {"own"},   {"part"},
    {"place"}, {"play"},  {"point"}, {"put"},   {"read"},  {"right"}, {"run"},   {"said"},  {"same"},
    {"saw"},   {"say"},   {"school"},{"see"},   {"set"},   {"she"},   {"should"},{"show"},  {"side"},
    {"small"}, {"so"},    {"some"},  {"sound"}, {"start"}, {"still"}, {"stop"},  {"such"},  {"take"},
    {"tell"},  {"than"},  {"that"},  {"the"},   {"their"}, {"them"},  {"then"},  {"there"}, {"these"},
    {"they"},  {"thing"}, {"think"}, {"this"},  {"those"}, {"three"}, {"through"},{"time"}, {"to"},
    {"too"},   {"took"},  {"turn"},  {"two"},   {"up"},    {"us"},    {"use"},   {"very"},  {"want"},
    {"was"},   {"water"}, {"way"},   {"we"},    {"well"},  {"went"},  {"were"},  {"what"},  {"when"},
    {"where"}, {"which"}, {"while"}, {"white"}, {"who"},   {"why"},   {"will"},  {"with"},  {"word"},
    {"work"},  {"world"}, {"would"}, {"write"}, {"year"},  {"you"},   {"your"},
};

const word_def_t *syllables_table(size_t *count)
{
    *count = sizeof(k_words) / sizeof(k_words[0]);
    return k_words;
}

const char *syllables_kind(void) { return "english common monosyllables (control)"; }

#endif
