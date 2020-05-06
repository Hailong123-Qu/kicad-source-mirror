/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2020 CERN
 *
 * @author Wayne Stambaugh <stambaughw@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

/**
 * @file sch_sexpr_parser.h
 * @brief Schematic and symbol library s-expression file format parser definitions.
 */

#ifndef __SCH_SEXPR_PARSER_H__
#define __SCH_SEXPR_PARSER_H__

#include <convert_to_biu.h>                      // IU_PER_MM
#include <math/util.h>                           // KiROUND, Clamp

#include <class_library.h>
#include <schematic_lexer.h>
#include <default_values.h>    // For some default values


class LIB_ARC;
class LIB_BEZIER;
class LIB_CIRCLE;
class LIB_ITEM;
class LIB_PIN;
class LIB_POLYLINE;
class LIB_TEXT;
class PAGE_INFO;
class SCH_BITMAP;
class SCH_BUS_WIRE_ENTRY;
class SCH_COMPONENT;
class SCH_FIELD;
class SCH_JUNCTION;
class SCH_LINE;
class SCH_NO_CONNECT;
class SCH_SCREEN;
class SCH_SHEET;
class SCH_SHEET_PIN;
class SCH_TEXT;
class TITLE_BLOCK;


/**
 * Simple container to manage line stroke parameters.
 */
class STROKE_PARAMS
{
public:
    int m_Width;
    PLOT_DASH_TYPE m_Type;
    COLOR4D m_Color;

    STROKE_PARAMS( int aWidth = Mils2iu( DEFAULT_LINE_THICKNESS ),
                   PLOT_DASH_TYPE aType = PLOT_DASH_TYPE::DEFAULT,
                   const COLOR4D& aColor = COLOR4D::UNSPECIFIED ) :
            m_Width( aWidth ),
            m_Type( aType ),
            m_Color( aColor )
    {
    }
};


/**
 * Simple container to manage fill parameters.
 */
class FILL_PARAMS
{
public:
    FILL_T m_FillType;
    COLOR4D m_Color;
};


/**
 * Object to parser s-expression symbol library and schematic file formats.
 */
class SCH_SEXPR_PARSER : public SCHEMATIC_LEXER
{
    int m_requiredVersion;  ///< Set to the symbol library file version required.
    int m_fieldId;          ///< The current field ID.
    int m_unit;             ///< The current unit being parsed.
    int m_convert;          ///< The current body style being parsed.
    wxString m_symbolName;  ///< The current symbol name.

    void parseHeader( TSCHEMATIC_T::T aHeaderType, int aFileVersion );

    inline long parseHex()
    {
        NextTok();
        return strtol( CurText(), NULL, 16 );
    }

    inline int parseInt()
    {
        return (int)strtol( CurText(), NULL, 10 );
    }

    inline int parseInt( const char* aExpected )
    {
        NeedNUMBER( aExpected );
        return parseInt();
    }

    /**
     * Parse the current token as an ASCII numeric string with possible leading
     * whitespace into a double precision floating point number.
     *
     * @throw IO_ERROR if an error occurs attempting to convert the current token.
     * @return The result of the parsed token.
     */
    double parseDouble();

    inline double parseDouble( const char* aExpected )
    {
        NeedNUMBER( aExpected );
        return parseDouble();
    }

    inline double parseDouble( TSCHEMATIC_T::T aToken )
    {
        return parseDouble( GetTokenText( aToken ) );
    }

    inline int parseInternalUnits()
    {
        auto retval = parseDouble() * IU_PER_MM;

        // Schematic internal units are represented as integers.  Any values that are
        // larger or smaller than the schematic units represent undefined behavior for
        // the system.  Limit values to the largest that can be displayed on the screen.
        double int_limit = std::numeric_limits<int>::max() * 0.7071; // 0.7071 = roughly 1/sqrt(2)

        return KiROUND( Clamp<double>( -int_limit, retval, int_limit ) );
    }

    inline int parseInternalUnits( const char* aExpected )
    {
        auto retval = parseDouble( aExpected ) * IU_PER_MM;

        double int_limit = std::numeric_limits<int>::max() * 0.7071;

        return KiROUND( Clamp<double>( -int_limit, retval, int_limit ) );
    }

    inline int parseInternalUnits( TSCHEMATIC_T::T aToken )
    {
        return parseInternalUnits( GetTokenText( aToken ) );
    }

    inline wxPoint parseXY()
    {
        wxPoint xy;

        xy.x = parseInternalUnits( "X coordinate" );
        xy.y = parseInternalUnits( "Y coordinate" );

        return xy;
    }

    /**
     * Parse stroke definition \a aStroke.
     *
     * @param aStrokeDef A reference to the #STROKE_PARAMS structure to write to.
     */
    void parseStroke( STROKE_PARAMS& aStroke );

    void parseFill( FILL_PARAMS& aFill );

    void parseEDA_TEXT( EDA_TEXT* aText );
    void parsePinNames( std::unique_ptr<LIB_PART>& aSymbol );

    void parseProperty( std::unique_ptr<LIB_PART>& aSymbol );

    LIB_ARC* parseArc();
    LIB_BEZIER* parseBezier();
    LIB_CIRCLE* parseCircle();
    LIB_PIN* parsePin();
    LIB_POLYLINE* parsePolyLine();
    LIB_RECTANGLE* parseRectangle();
    LIB_TEXT* parseText();

    void parsePAGE_INFO( PAGE_INFO& aPageInfo );
    void parseTITLE_BLOCK( TITLE_BLOCK& aTitleBlock );
    void parseSchSymbolInstances( SCH_SCREEN* aScreen );

    SCH_SHEET_PIN* parseSchSheetPin( SCH_SHEET* aSheet );
    SCH_FIELD* parseSchField( SCH_COMPONENT* aParentSymbol );
    SCH_COMPONENT* parseSchematicSymbol();
    SCH_BITMAP* parseImage();
    SCH_SHEET* parseSheet();
    SCH_JUNCTION* parseJunction();
    SCH_NO_CONNECT* parseNoConnect();
    SCH_BUS_WIRE_ENTRY* parseBusEntry();
    SCH_LINE* parseLine();
    SCH_TEXT* parseSchText();
    void parseBusAlias( SCH_SCREEN* aScreen );

public:
    SCH_SEXPR_PARSER( LINE_READER* aLineReader = nullptr );

    void ParseLib( LIB_PART_MAP& aSymbolLibMap );

    LIB_PART* ParseSymbol( LIB_PART_MAP& aSymbolLibMap, bool aIsSchematicLib = false );

    LIB_ITEM* ParseDrawItem();

    /**
     * Parse a single schematic file into \a aSheet.
     *
     * @note This does not load any sub-sheets or decent complex sheet hierarchies.
     *
     * @param aSheet The #SCH_SHEET object to store the parsed schematic file.
     */
    void ParseSchematic( SCH_SHEET* aSheet );

    /**
     * Return whether a version number, if any was parsed, was too recent
     */
    bool IsTooRecent() const;
};

#endif    // __SCH_SEXPR_PARSER_H__
