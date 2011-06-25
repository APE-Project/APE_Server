/*
 * Copyright (C) 2009 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "jsinttypes.h"
#include "RegexCompiler.h"

#include "RegexPattern.h"

using namespace WTF;

namespace JSC { namespace Yarr {

#include "RegExpJitTables.h"

class CharacterClassConstructor {
public:
    CharacterClassConstructor(bool isCaseInsensitive = false)
        : m_isCaseInsensitive(isCaseInsensitive)
    {
    }
    
    void reset()
    {
        m_matches.clear();
        m_ranges.clear();
        m_matchesUnicode.clear();
        m_rangesUnicode.clear();
    }

    void append(const CharacterClass* other)
    {
        for (size_t i = 0; i < other->m_matches.length(); ++i)
            addSorted(m_matches, other->m_matches[i]);
        for (size_t i = 0; i < other->m_ranges.length(); ++i)
            addSortedRange(m_ranges, other->m_ranges[i].begin, other->m_ranges[i].end);
        for (size_t i = 0; i < other->m_matchesUnicode.length(); ++i)
            addSorted(m_matchesUnicode, other->m_matchesUnicode[i]);
        for (size_t i = 0; i < other->m_rangesUnicode.length(); ++i)
            addSortedRange(m_rangesUnicode, other->m_rangesUnicode[i].begin, other->m_rangesUnicode[i].end);
    }

    void putChar(UChar ch)
    {
        if (ch <= 0x7f) {
            if (m_isCaseInsensitive && isASCIIAlpha(ch)) {
                addSorted(m_matches, toASCIIUpper(ch));
                addSorted(m_matches, toASCIILower(ch));
            } else
                addSorted(m_matches, ch);
        } else {
            UChar upper, lower;
            if (m_isCaseInsensitive && ((upper = Unicode::toUpper(ch)) != (lower = Unicode::toLower(ch)))) {
                addSorted(m_matchesUnicode, upper);
                addSorted(m_matchesUnicode, lower);
            } else
                addSorted(m_matchesUnicode, ch);
        }
    }

    // returns true if this character has another case, and 'ch' is the upper case form.
    static inline bool isUnicodeUpper(UChar ch)
    {
        return ch != Unicode::toLower(ch);
    }

    // returns true if this character has another case, and 'ch' is the lower case form.
    static inline bool isUnicodeLower(UChar ch)
    {
        return ch != Unicode::toUpper(ch);
    }

    void putRange(UChar lo, UChar hi)
    {
        if (lo <= 0x7f) {
            char asciiLo = lo;
            char asciiHi = JS_MIN(hi, (UChar)0x7f);
            addSortedRange(m_ranges, lo, asciiHi);
            
            if (m_isCaseInsensitive) {
                if ((asciiLo <= 'Z') && (asciiHi >= 'A'))
                    addSortedRange(m_ranges, JS_MAX(asciiLo, 'A')+('a'-'A'), JS_MIN(asciiHi, 'Z')+('a'-'A'));
                if ((asciiLo <= 'z') && (asciiHi >= 'a'))
                    addSortedRange(m_ranges, JS_MAX(asciiLo, 'a')+('A'-'a'), JS_MIN(asciiHi, 'z')+('A'-'a'));
            }
        }
        if (hi >= 0x80) {
            uint32 unicodeCurr = JS_MAX(lo, (UChar)0x80);
            addSortedRange(m_rangesUnicode, unicodeCurr, hi);
            
            if (m_isCaseInsensitive) {
                while (unicodeCurr <= hi) {
                    // If the upper bound of the range (hi) is 0xffff, the increments to
                    // unicodeCurr in this loop may take it to 0x10000.  This is fine
                    // (if so we won't re-enter the loop, since the loop condition above
                    // will definitely fail) - but this does mean we cannot use a UChar
                    // to represent unicodeCurr, we must use a 32-bit value instead.
                    JS_ASSERT(unicodeCurr <= 0xffff);

                    if (isUnicodeUpper(unicodeCurr)) {
                        UChar lowerCaseRangeBegin = Unicode::toLower(unicodeCurr);
                        UChar lowerCaseRangeEnd = lowerCaseRangeBegin;
                        while ((++unicodeCurr <= hi) && isUnicodeUpper(unicodeCurr) && (Unicode::toLower(unicodeCurr) == (lowerCaseRangeEnd + 1)))
                            lowerCaseRangeEnd++;
                        addSortedRange(m_rangesUnicode, lowerCaseRangeBegin, lowerCaseRangeEnd);
                    } else if (isUnicodeLower(unicodeCurr)) {
                        UChar upperCaseRangeBegin = Unicode::toUpper(unicodeCurr);
                        UChar upperCaseRangeEnd = upperCaseRangeBegin;
                        while ((++unicodeCurr <= hi) && isUnicodeLower(unicodeCurr) && (Unicode::toUpper(unicodeCurr) == (upperCaseRangeEnd + 1)))
                            upperCaseRangeEnd++;
                        addSortedRange(m_rangesUnicode, upperCaseRangeBegin, upperCaseRangeEnd);
                    } else
                        ++unicodeCurr;
                }
            }
        }
    }

    CharacterClass* charClass()
    {
        // FIXME: bug 574459 -- no NULL check
        CharacterClass* characterClass = js_new<CharacterClass>((CharacterClassTable*)NULL);

        characterClass->m_matches.append(m_matches);
        characterClass->m_ranges.append(m_ranges);
        characterClass->m_matchesUnicode.append(m_matchesUnicode);
        characterClass->m_rangesUnicode.append(m_rangesUnicode);

        reset();

        return characterClass;
    }

private:
    typedef js::Vector<UChar, 0, js::SystemAllocPolicy> UChars;
    typedef js::Vector<CharacterRange, 0, js::SystemAllocPolicy> CharacterRanges;
    void addSorted(UChars& matches, UChar ch)
    {
        unsigned pos = 0;
        unsigned range = matches.length();

        // binary chop, find position to insert char.
        while (range) {
            unsigned index = range >> 1;

            int val = matches[pos+index] - ch;
            if (!val)
                return;
            else if (val > 0)
                range = index;
            else {
                pos += (index+1);
                range -= (index+1);
            }
        }
        
        if (pos == matches.length())
            matches.append(ch);
        else
            matches.insert(matches.begin() + pos, ch);
    }

    void addSortedRange(CharacterRanges& ranges, UChar lo, UChar hi)
    {
        unsigned end = ranges.length();
        
        // Simple linear scan - I doubt there are that many ranges anyway...
        // feel free to fix this with something faster (eg binary chop).
        for (unsigned i = 0; i < end; ++i) {
            // does the new range fall before the current position in the array
            if (hi < ranges[i].begin) {
                // optional optimization: concatenate appending ranges? - may not be worthwhile.
                if (hi == (ranges[i].begin - 1)) {
                    ranges[i].begin = lo;
                    return;
                }
                ranges.insert(ranges.begin() + i, CharacterRange(lo, hi));
                return;
            }
            // Okay, since we didn't hit the last case, the end of the new range is definitely at or after the begining
            // If the new range start at or before the end of the last range, then the overlap (if it starts one after the
            // end of the last range they concatenate, which is just as good.
            if (lo <= (ranges[i].end + 1)) {
                // found an intersect! we'll replace this entry in the array.
                ranges[i].begin = JS_MIN(ranges[i].begin, lo);
                ranges[i].end = JS_MAX(ranges[i].end, hi);

                // now check if the new range can subsume any subsequent ranges.
                unsigned next = i+1;
                // each iteration of the loop we will either remove something from the list, or break the loop.
                while (next < ranges.length()) {
                    if (ranges[next].begin <= (ranges[i].end + 1)) {
                        // the next entry now overlaps / concatenates this one.
                        ranges[i].end = JS_MAX(ranges[i].end, ranges[next].end);
                        ranges.erase(ranges.begin() + next);
                    } else
                        break;
                }
                
                return;
            }
        }

        // CharacterRange comes after all existing ranges.
        ranges.append(CharacterRange(lo, hi));
    }

    bool m_isCaseInsensitive;

    UChars m_matches;
    CharacterRanges m_ranges;
    UChars m_matchesUnicode;
    CharacterRanges m_rangesUnicode;
};

class RegexPatternConstructor {
public:
    RegexPatternConstructor(RegexPattern& pattern)
        : m_pattern(pattern)
        , m_characterClassConstructor(pattern.m_ignoreCase)
    {
    }

    ~RegexPatternConstructor()
    {
    }

    void reset()
    {
        m_pattern.reset();
        m_characterClassConstructor.reset();
    }
    
    void assertionBOL()
    {
        m_alternative->m_terms.append(PatternTerm::BOL());
    }
    void assertionEOL()
    {
        m_alternative->m_terms.append(PatternTerm::EOL());
    }
    void assertionWordBoundary(bool invert)
    {
        m_alternative->m_terms.append(PatternTerm::WordBoundary(invert));
    }

    void atomPatternCharacter(UChar ch)
    {
        // We handle case-insensitive checking of unicode characters which do have both
        // cases by handling them as if they were defined using a CharacterClass.
        if (m_pattern.m_ignoreCase && !isASCII(ch) && (Unicode::toUpper(ch) != Unicode::toLower(ch))) {
            atomCharacterClassBegin();
            atomCharacterClassAtom(ch);
            atomCharacterClassEnd();
        } else
            m_alternative->m_terms.append(PatternTerm(ch));
    }

    void atomBuiltInCharacterClass(BuiltInCharacterClassID classID, bool invert)
    {
        switch (classID) {
        case DigitClassID:
            m_alternative->m_terms.append(PatternTerm(m_pattern.digitsCharacterClass(), invert));
            break;
        case SpaceClassID:
            m_alternative->m_terms.append(PatternTerm(m_pattern.spacesCharacterClass(), invert));
            break;
        case WordClassID:
            m_alternative->m_terms.append(PatternTerm(m_pattern.wordcharCharacterClass(), invert));
            break;
        case NewlineClassID:
            m_alternative->m_terms.append(PatternTerm(m_pattern.newlineCharacterClass(), invert));
            break;
        }
    }

    void atomCharacterClassBegin(bool invert = false)
    {
        m_invertCharacterClass = invert;
    }

    void atomCharacterClassAtom(UChar ch)
    {
        m_characterClassConstructor.putChar(ch);
    }

    void atomCharacterClassRange(UChar begin, UChar end)
    {
        m_characterClassConstructor.putRange(begin, end);
    }

    void atomCharacterClassBuiltIn(BuiltInCharacterClassID classID, bool invert)
    {
        JS_ASSERT(classID != NewlineClassID);

        switch (classID) {
        case DigitClassID:
            m_characterClassConstructor.append(invert ? m_pattern.nondigitsCharacterClass() : m_pattern.digitsCharacterClass());
            break;
        
        case SpaceClassID:
            m_characterClassConstructor.append(invert ? m_pattern.nonspacesCharacterClass() : m_pattern.spacesCharacterClass());
            break;
        
        case WordClassID:
            m_characterClassConstructor.append(invert ? m_pattern.nonwordcharCharacterClass() : m_pattern.wordcharCharacterClass());
            break;
        
        default:
            JS_NOT_REACHED("Invalid character class.");
        }
    }

    void atomCharacterClassEnd()
    {
        CharacterClass* newCharacterClass = m_characterClassConstructor.charClass();
        m_pattern.m_userCharacterClasses.append(newCharacterClass);
        m_alternative->m_terms.append(PatternTerm(newCharacterClass, m_invertCharacterClass));
    }

    void atomParenthesesSubpatternBegin(bool capture = true)
    {
        unsigned subpatternId = m_pattern.m_numSubpatterns + 1;
        if (capture)
            m_pattern.m_numSubpatterns++;

        // FIXME: bug 574459 -- no NULL check
        PatternDisjunction* parenthesesDisjunction = js_new<PatternDisjunction>(m_alternative);
        m_pattern.m_disjunctions.append(parenthesesDisjunction);
        m_alternative->m_terms.append(PatternTerm(PatternTerm::TypeParenthesesSubpattern, subpatternId, parenthesesDisjunction, capture));
        m_alternative = parenthesesDisjunction->addNewAlternative();
    }

    void atomParentheticalAssertionBegin(bool invert = false)
    {
        // FIXME: bug 574459 -- no NULL check
        PatternDisjunction* parenthesesDisjunction = js_new<PatternDisjunction>(m_alternative);
        m_pattern.m_disjunctions.append(parenthesesDisjunction);
        m_alternative->m_terms.append(PatternTerm(PatternTerm::TypeParentheticalAssertion, m_pattern.m_numSubpatterns + 1, parenthesesDisjunction, invert));
        m_alternative = parenthesesDisjunction->addNewAlternative();
    }

    void atomParenthesesEnd()
    {
        JS_ASSERT(m_alternative->m_parent);
        JS_ASSERT(m_alternative->m_parent->m_parent);
        m_alternative = m_alternative->m_parent->m_parent;
        
        m_alternative->lastTerm().parentheses.lastSubpatternId = m_pattern.m_numSubpatterns;
    }

    void atomBackReference(unsigned subpatternId)
    {
        JS_ASSERT(subpatternId);
        m_pattern.m_containsBackreferences = true;
        m_pattern.m_maxBackReference = JS_MAX(m_pattern.m_maxBackReference, subpatternId);

        if (subpatternId > m_pattern.m_numSubpatterns) {
            m_alternative->m_terms.append(PatternTerm::ForwardReference());
            return;
        }

        PatternAlternative* currentAlternative = m_alternative;
        JS_ASSERT(currentAlternative);

        // Note to self: if we waited until the AST was baked, we could also remove forwards refs 
        while ((currentAlternative = currentAlternative->m_parent->m_parent)) {
            PatternTerm& term = currentAlternative->lastTerm();
            JS_ASSERT((term.type == PatternTerm::TypeParenthesesSubpattern) || (term.type == PatternTerm::TypeParentheticalAssertion));

            if ((term.type == PatternTerm::TypeParenthesesSubpattern) && term.invertOrCapture && (subpatternId == term.subpatternId)) {
                m_alternative->m_terms.append(PatternTerm::ForwardReference());
                return;
            }
        }

        m_alternative->m_terms.append(PatternTerm(subpatternId));
    }

    PatternDisjunction* copyDisjunction(PatternDisjunction* disjunction)
    {
        // FIXME: bug 574459 -- no NULL check
        PatternDisjunction* newDisjunction = js_new<PatternDisjunction>();

        newDisjunction->m_parent = disjunction->m_parent;
        for (unsigned alt = 0; alt < disjunction->m_alternatives.length(); ++alt) {
            PatternAlternative* alternative = disjunction->m_alternatives[alt];
            PatternAlternative* newAlternative = newDisjunction->addNewAlternative();
            for (unsigned i = 0; i < alternative->m_terms.length(); ++i)
                newAlternative->m_terms.append(copyTerm(alternative->m_terms[i]));
        }

        m_pattern.m_disjunctions.append(newDisjunction);
        return newDisjunction;
    }

    PatternTerm copyTerm(PatternTerm& term)
    {
        if ((term.type != PatternTerm::TypeParenthesesSubpattern) && (term.type != PatternTerm::TypeParentheticalAssertion))
            return PatternTerm(term);

        PatternTerm termCopy = term;
        termCopy.parentheses.disjunction = copyDisjunction(termCopy.parentheses.disjunction);
        return termCopy;
    }

    void quantifyAtom(unsigned min, unsigned max, bool greedy)
    {
        JS_ASSERT(min <= max);
        JS_ASSERT(m_alternative->m_terms.length());

        if (!max) {
            m_alternative->removeLastTerm();
            return;
        }

        PatternTerm& term = m_alternative->lastTerm();
        JS_ASSERT(term.type > PatternTerm::TypeAssertionWordBoundary);
        JS_ASSERT((term.quantityCount == 1) && (term.quantityType == QuantifierFixedCount));

        // For any assertion with a zero minimum, not matching is valid and has no effect,
        // remove it.  Otherwise, we need to match as least once, but there is no point
        // matching more than once, so remove the quantifier.  It is not entirely clear
        // from the spec whether or not this behavior is correct, but I believe this
        // matches Firefox. :-/
        if (term.type == PatternTerm::TypeParentheticalAssertion) {
            if (!min)
                m_alternative->removeLastTerm();
            return;
        }

        if (min == 0)
            term.quantify(max, greedy   ? QuantifierGreedy : QuantifierNonGreedy);
        else if (min == max)
            term.quantify(min, QuantifierFixedCount);
        else {
            term.quantify(min, QuantifierFixedCount);
            m_alternative->m_terms.append(copyTerm(term));
            // NOTE: this term is interesting from an analysis perspective, in that it can be ignored.....
            m_alternative->lastTerm().quantify((max == UINT_MAX) ? max : max - min, greedy ? QuantifierGreedy : QuantifierNonGreedy);
            if (m_alternative->lastTerm().type == PatternTerm::TypeParenthesesSubpattern)
                m_alternative->lastTerm().parentheses.isCopy = true;
        }
    }

    void disjunction()
    {
        m_alternative = m_alternative->m_parent->addNewAlternative();
    }

    void regexBegin()
    {
        // FIXME: bug 574459 -- no NULL check
        m_pattern.m_body = js_new<PatternDisjunction>();
        m_alternative = m_pattern.m_body->addNewAlternative();
        m_pattern.m_disjunctions.append(m_pattern.m_body);
    }
    void regexEnd()
    {
    }
    void regexError()
    {
    }

    unsigned setupAlternativeOffsets(PatternAlternative* alternative, unsigned currentCallFrameSize, unsigned initialInputPosition)
    {
        alternative->m_hasFixedSize = true;
        unsigned currentInputPosition = initialInputPosition;

        for (unsigned i = 0; i < alternative->m_terms.length(); ++i) {
            PatternTerm& term = alternative->m_terms[i];

            switch (term.type) {
            case PatternTerm::TypeAssertionBOL:
            case PatternTerm::TypeAssertionEOL:
            case PatternTerm::TypeAssertionWordBoundary:
                term.inputPosition = currentInputPosition;
                break;

            case PatternTerm::TypeBackReference:
                term.inputPosition = currentInputPosition;
                term.frameLocation = currentCallFrameSize;
                currentCallFrameSize += RegexStackSpaceForBackTrackInfoBackReference;
                alternative->m_hasFixedSize = false;
                break;

            case PatternTerm::TypeForwardReference:
                break;

            case PatternTerm::TypePatternCharacter:
                term.inputPosition = currentInputPosition;
                if (term.quantityType != QuantifierFixedCount) {
                    term.frameLocation = currentCallFrameSize;
                    currentCallFrameSize += RegexStackSpaceForBackTrackInfoPatternCharacter;
                    alternative->m_hasFixedSize = false;
                } else
                    currentInputPosition += term.quantityCount;
                break;

            case PatternTerm::TypeCharacterClass:
                term.inputPosition = currentInputPosition;
                if (term.quantityType != QuantifierFixedCount) {
                    term.frameLocation = currentCallFrameSize;
                    currentCallFrameSize += RegexStackSpaceForBackTrackInfoCharacterClass;
                    alternative->m_hasFixedSize = false;
                } else
                    currentInputPosition += term.quantityCount;
                break;

            case PatternTerm::TypeParenthesesSubpattern:
                // Note: for fixed once parentheses we will ensure at least the minimum is available; others are on their own.
                term.frameLocation = currentCallFrameSize;
                if (term.quantityCount == 1 && !term.parentheses.isCopy) {
                    if (term.quantityType != QuantifierFixedCount)
                        currentCallFrameSize += RegexStackSpaceForBackTrackInfoParenthesesOnce;
                    currentCallFrameSize = setupDisjunctionOffsets(term.parentheses.disjunction, currentCallFrameSize, currentInputPosition);
                    // If quantity is fixed, then pre-check its minimum size.
                    if (term.quantityType == QuantifierFixedCount)
                        currentInputPosition += term.parentheses.disjunction->m_minimumSize;
                    term.inputPosition = currentInputPosition;
                } else if (term.parentheses.isTerminal) {
                    currentCallFrameSize += RegexStackSpaceForBackTrackInfoParenthesesTerminal;
                    currentCallFrameSize = setupDisjunctionOffsets(term.parentheses.disjunction, currentCallFrameSize, currentInputPosition);
                    term.inputPosition = currentInputPosition;
                } else {
                    term.inputPosition = currentInputPosition;
                    setupDisjunctionOffsets(term.parentheses.disjunction, 0, currentInputPosition);
                    currentCallFrameSize += RegexStackSpaceForBackTrackInfoParentheses;
                }
                // Fixed count of 1 could be accepted, if they have a fixed size *AND* if all alternatives are of the same length.
                alternative->m_hasFixedSize = false;
                break;

            case PatternTerm::TypeParentheticalAssertion:
                term.inputPosition = currentInputPosition;
                term.frameLocation = currentCallFrameSize;
                currentCallFrameSize = setupDisjunctionOffsets(term.parentheses.disjunction, currentCallFrameSize + RegexStackSpaceForBackTrackInfoParentheticalAssertion, currentInputPosition);
                break;
            }
        }

        alternative->m_minimumSize = currentInputPosition - initialInputPosition;
        return currentCallFrameSize;
    }

    unsigned setupDisjunctionOffsets(PatternDisjunction* disjunction, unsigned initialCallFrameSize, unsigned initialInputPosition)
    {
        if ((disjunction != m_pattern.m_body) && (disjunction->m_alternatives.length() > 1))
            initialCallFrameSize += RegexStackSpaceForBackTrackInfoAlternative;

        unsigned minimumInputSize = UINT_MAX;
        unsigned maximumCallFrameSize = 0;
        bool hasFixedSize = true;

        for (unsigned alt = 0; alt < disjunction->m_alternatives.length(); ++alt) {
            PatternAlternative* alternative = disjunction->m_alternatives[alt];
            unsigned currentAlternativeCallFrameSize = setupAlternativeOffsets(alternative, initialCallFrameSize, initialInputPosition);
            minimumInputSize = JS_MIN(minimumInputSize, alternative->m_minimumSize);
            maximumCallFrameSize = JS_MAX(maximumCallFrameSize, currentAlternativeCallFrameSize);
            hasFixedSize &= alternative->m_hasFixedSize;
        }
        
        JS_ASSERT(minimumInputSize != UINT_MAX);
        JS_ASSERT(maximumCallFrameSize >= initialCallFrameSize);

        disjunction->m_hasFixedSize = hasFixedSize;
        disjunction->m_minimumSize = minimumInputSize;
        disjunction->m_callFrameSize = maximumCallFrameSize;
        return maximumCallFrameSize;
    }

    void setupOffsets()
    {
        setupDisjunctionOffsets(m_pattern.m_body, 0, 0);
    }

    // This optimization identifies sets of parentheses that we will never need to backtrack.
    // In these cases we do not need to store state from prior iterations.
    // We can presently avoid backtracking for:
    //   * a set of parens at the end of the regular expression (last term in any of the alternatives of the main body disjunction).
    //   * where the parens are non-capturing, and quantified unbounded greedy (*).
    //   * where the parens do not contain any capturing subpatterns.
    void checkForTerminalParentheses()
    {
        // This check is much too crude; should be just checking whether the candidate
        // node contains nested capturing subpatterns, not the whole expression!
        if (m_pattern.m_numSubpatterns)
            return;

        js::Vector<PatternAlternative*, 0, js::SystemAllocPolicy>& alternatives = m_pattern.m_body->m_alternatives;
        for (unsigned i =0; i < alternatives.length(); ++i) {
            js::Vector<PatternTerm, 0, js::SystemAllocPolicy>& terms = alternatives[i]->m_terms;
            if (terms.length()) {
                PatternTerm& term = terms.back();
                if (term.type == PatternTerm::TypeParenthesesSubpattern
                    && term.quantityType == QuantifierGreedy
                    && term.quantityCount == UINT_MAX
                    && !term.capture())
                    term.parentheses.isTerminal = true;
            }
        }
    }

private:
    RegexPattern& m_pattern;
    PatternAlternative* m_alternative;
    CharacterClassConstructor m_characterClassConstructor;
    bool m_invertCharacterClass;
};


int compileRegex(const UString& patternString, RegexPattern& pattern)
{
    RegexPatternConstructor constructor(pattern);

    if (int error = parse(constructor, patternString))
        return error;
    
    // If the pattern contains illegal backreferences reset & reparse.
    // Quoting Netscape's "What's new in JavaScript 1.2",
    //      "Note: if the number of left parentheses is less than the number specified
    //       in \#, the \# is taken as an octal escape as described in the next row."
    if (pattern.containsIllegalBackReference()) {
        unsigned numSubpatterns = pattern.m_numSubpatterns;

        constructor.reset();
#ifdef DEBUG
        int error =
#endif
            parse(constructor, patternString, numSubpatterns);

        JS_ASSERT(!error);
        JS_ASSERT(numSubpatterns == pattern.m_numSubpatterns);
    }

    constructor.checkForTerminalParentheses();
    constructor.setupOffsets();

    return 0;
}


} }
