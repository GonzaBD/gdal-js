/******************************************************************************
 * $Id$
 *
 * Project:  GML Reader
 * Purpose:  Private Declarations for OGR free GML Reader code.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2002, Frank Warmerdam
 * Copyright (c) 2008-2013, Even Rouault <even dot rouault at mines-paris dot org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#ifndef CPL_GMLREADERP_H_INCLUDED
#define CPL_GMLREADERP_H_INCLUDED

#if defined(HAVE_XERCES)

#include "xercesc_headers.h"

#endif /* HAVE_XERCES */

#include "gmlreader.h"
#include "ogr_api.h"
#include "cpl_vsi.h"
#include "cpl_multiproc.h"
#include "gmlutils.h"

#include <string>
#include <vector>

#define PARSER_BUF_SIZE (10*8192)

class GMLReader;

typedef struct _GeometryNamesStruct GeometryNamesStruct;

/************************************************************************/
/*                        GFSTemplateList                               */
/************************************************************************/

class GFSTemplateItem;

class GFSTemplateList
{
private:
    bool            m_bSequentialLayers;
    GFSTemplateItem *pFirst;
    GFSTemplateItem *pLast;
    GFSTemplateItem *Insert( const char *pszName );
public:
                    GFSTemplateList( void );
                    ~GFSTemplateList();
    void            Update( const char *pszName, int bHasGeom );
    GFSTemplateItem *GetFirst() { return pFirst; }
    bool            HaveSequentialLayers() { return m_bSequentialLayers; }
    int             GetClassCount();
};

void gmlUpdateFeatureClasses ( GFSTemplateList *pCC,
                               GMLReader *pReader,
                               int *pnHasSequentialLayers );

/************************************************************************/
/*                              GMLHandler                              */
/************************************************************************/

#define STACK_SIZE 5

typedef enum
{
    STATE_TOP,
    STATE_DEFAULT,
    STATE_FEATURE,
    STATE_PROPERTY,
    STATE_FEATUREPROPERTY,
    STATE_GEOMETRY,
    STATE_IGNORED_FEATURE,
    STATE_BOUNDED_BY,
    STATE_CITYGML_ATTRIBUTE
} HandlerState;

typedef struct
{
    CPLXMLNode* psNode;
    CPLXMLNode* psLastChild;
} NodeLastChild;


typedef enum
{
    APPSCHEMA_GENERIC,
    APPSCHEMA_CITYGML,
    APPSCHEMA_AIXM,
    APPSCHEMA_MTKGML /* format of National Land Survey Finnish */
} GMLAppSchemaType;

class GMLHandler
{
    char      *m_pszCurField;
    unsigned int m_nCurFieldAlloc;
    unsigned int m_nCurFieldLen;
    bool       m_bInCurField;
    int        m_nAttributeIndex;
    int        m_nAttributeDepth;


    char      *m_pszGeometry;
    unsigned int m_nGeomAlloc;
    unsigned int m_nGeomLen;
    int        m_nGeometryDepth;
    bool       m_bAlreadyFoundGeometry;
    int        m_nGeometryPropertyIndex;

    int        m_nDepth;
    int        m_nDepthFeature;

    int        m_inBoundedByDepth;

    char      *m_pszCityGMLGenericAttrName;
    int        m_inCityGMLGenericAttrDepth;

    bool       m_bReportHref;
    char      *m_pszHref;
    char      *m_pszUom;
    char      *m_pszValue;
    char      *m_pszKieli;

    GeometryNamesStruct* pasGeometryNames;

    std::vector<NodeLastChild> apsXMLNode;

    int        m_nSRSDimensionIfMissing;

    OGRErr     startElementTop(const char *pszName, int nLenName, void* attr);

    OGRErr     endElementIgnoredFeature();

    OGRErr     startElementBoundedBy(const char *pszName, int nLenName, void* attr);
    OGRErr     endElementBoundedBy();

    OGRErr     startElementFeatureAttribute(const char *pszName, int nLenName, void* attr);
    OGRErr     endElementFeature();

    OGRErr     startElementCityGMLGenericAttr(const char *pszName, int nLenName, void* attr);
    OGRErr     endElementCityGMLGenericAttr();

    OGRErr     startElementGeometry(const char *pszName, int nLenName, void* attr);
    CPLXMLNode* ParseAIXMElevationPoint(CPLXMLNode*);
    OGRErr     endElementGeometry();
    OGRErr     dataHandlerGeometry(const char *data, int nLen);

    OGRErr     endElementAttribute();
    OGRErr     dataHandlerAttribute(const char *data, int nLen);

    OGRErr     startElementDefault(const char *pszName, int nLenName, void* attr);
    OGRErr     endElementDefault();

    OGRErr     startElementFeatureProperty(const char *pszName, int nLenName, void* attr);
    OGRErr     endElementFeatureProperty();

    void       DealWithAttributes(const char *pszName, int nLenName, void* attr );
    bool       IsConditionMatched(const char* pszCondition, void* attr);
    int        FindRealPropertyByCheckingConditions(int nIdx, void* attr);

protected:
    GMLReader  *m_poReader;
    GMLAppSchemaType eAppSchemaType;

    int              nStackDepth;
    HandlerState     stateStack[STACK_SIZE];

    std::string      osFID;
    virtual const char* GetFID(void* attr) = 0;

    virtual CPLXMLNode* AddAttributes(CPLXMLNode* psNode, void* attr) = 0;

    OGRErr      startElement(const char *pszName, int nLenName, void* attr);
    OGRErr      endElement();
    OGRErr      dataHandler(const char *data, int nLen);

    bool       IsGeometryElement( const char *pszElement );

public:
    GMLHandler( GMLReader *poReader );
    virtual ~GMLHandler();

    virtual char*       GetAttributeValue(void* attr, const char* pszAttributeName) = 0;
    virtual char*       GetAttributeByIdx(void* attr, unsigned int idx, char** ppszKey) = 0;
};


#if defined(HAVE_XERCES)

/************************************************************************/
/*                        GMLBinInputStream                             */
/************************************************************************/
class GMLBinInputStream : public BinInputStream
{
    VSILFILE* fp;
    XMLCh emptyString;

public :

             GMLBinInputStream(VSILFILE* fp);
    virtual ~GMLBinInputStream();

#if XERCES_VERSION_MAJOR >= 3
    virtual XMLFilePos curPos() const;
    virtual XMLSize_t readBytes(XMLByte* const toFill, const XMLSize_t maxToRead);
    virtual const XMLCh* getContentType() const ;
#else
    virtual unsigned int curPos() const;
    virtual unsigned int readBytes(XMLByte* const toFill, const unsigned int maxToRead);
#endif
};

/************************************************************************/
/*                           GMLInputSource                             */
/************************************************************************/

class GMLInputSource : public InputSource
{
    GMLBinInputStream* binInputStream;

public:
             GMLInputSource(VSILFILE* fp,
                            MemoryManager* const manager = XMLPlatformUtils::fgMemoryManager);
    virtual ~GMLInputSource();

    virtual BinInputStream* makeStream() const;
};


/************************************************************************/
/*          XMLCh / char translation functions - trstring.cpp           */
/************************************************************************/
int tr_strcmp( const char *, const XMLCh * );
void tr_strcpy( XMLCh *, const char * );
void tr_strcpy( char *, const XMLCh * );
char *tr_strdup( const XMLCh * );
int tr_strlen( const XMLCh * );

/************************************************************************/
/*                         GMLXercesHandler                             */
/************************************************************************/
class GMLXercesHandler : public DefaultHandler, public GMLHandler
{
    int        m_nEntityCounter;

public:
    GMLXercesHandler( GMLReader *poReader );

    void startElement(
        const   XMLCh* const    uri,
        const   XMLCh* const    localname,
        const   XMLCh* const    qname,
        const   Attributes& attrs
    );
    void endElement(
        const   XMLCh* const    uri,
        const   XMLCh* const    localname,
        const   XMLCh* const    qname
    );
#if XERCES_VERSION_MAJOR >= 3
    void characters( const XMLCh *const chars,
                     const XMLSize_t length );
#else
    void characters( const XMLCh *const chars,
                     const unsigned int length );
#endif

    void fatalError(const SAXParseException&);

    void startEntity (const XMLCh *const name);

    virtual const char* GetFID(void* attr);
    virtual CPLXMLNode* AddAttributes(CPLXMLNode* psNode, void* attr);
    virtual char*       GetAttributeValue(void* attr, const char* pszAttributeName);
    virtual char*       GetAttributeByIdx(void* attr, unsigned int idx, char** ppszKey);
};

#endif


#if defined(HAVE_EXPAT)

#include "ogr_expat.h"

/************************************************************************/
/*                           GMLExpatHandler                            */
/************************************************************************/
class GMLExpatHandler : public GMLHandler
{
    XML_Parser m_oParser;
    bool       m_bStopParsing;
    int        m_nDataHandlerCounter;

public:
    GMLExpatHandler( GMLReader *poReader, XML_Parser oParser );

    bool        HasStoppedParsing() { return m_bStopParsing; }

    void        ResetDataHandlerCounter() { m_nDataHandlerCounter = 0; }

    virtual const char* GetFID(void* attr);
    virtual CPLXMLNode* AddAttributes(CPLXMLNode* psNode, void* attr);
    virtual char*       GetAttributeValue(void* attr, const char* pszAttributeName);
    virtual char*       GetAttributeByIdx(void* attr, unsigned int idx, char** ppszKey);

    static void XMLCALL startElementCbk(void *pUserData, const char *pszName,
                                        const char **ppszAttr);

    static void XMLCALL endElementCbk(void *pUserData, const char *pszName);

    static void XMLCALL dataHandlerCbk(void *pUserData, const char *data, int nLen);
};

#endif

/************************************************************************/
/*                             GMLReadState                             */
/************************************************************************/

class GMLReadState
{
    std::vector<std::string> aosPathComponents;

public:
    GMLReadState();
    ~GMLReadState();

    void        PushPath( const char *pszElement, int nLen = -1 );
    void        PopPath();

    const char  *GetLastComponent() const {
        return ( m_nPathLength == 0 ) ? "" : aosPathComponents[m_nPathLength-1].c_str();
    }


    size_t GetLastComponentLen() const {
        return ( m_nPathLength == 0 ) ? 0: aosPathComponents[m_nPathLength-1].size();
    }

    void        Reset();

    GMLFeature  *m_poFeature;
    GMLReadState *m_poParentState;

    std::string  osPath; // element path ... | as separator.
    int          m_nPathLength;
};

/************************************************************************/
/*                              GMLReader                               */
/************************************************************************/

typedef enum
{
    OGRGML_XERCES_UNINITIALIZED,
    OGRGML_XERCES_INIT_FAILED,
    OGRGML_XERCES_INIT_SUCCESSFUL
} OGRGMLXercesState;

class GMLReader : public IGMLReader
{
private:
    static OGRGMLXercesState    m_eXercesInitState;
    static int    m_nInstanceCount;
    bool          m_bClassListLocked;

    int         m_nClassCount;
    GMLFeatureClass **m_papoClass;
    bool          m_bLookForClassAtAnyLevel;

    char          *m_pszFilename;

    bool           bUseExpatReader;

    GMLHandler    *m_poGMLHandler;

#if defined(HAVE_XERCES)
    SAX2XMLReader *m_poSAXReader;
    XMLPScanToken m_oToFill;
    GMLFeature   *m_poCompleteFeature;
    GMLInputSource *m_GMLInputSource;
    bool          m_bEOF;
    bool          SetupParserXerces();
    GMLFeature   *NextFeatureXerces();
#endif

#if defined(HAVE_EXPAT)
    XML_Parser    oParser;
    GMLFeature ** ppoFeatureTab;
    int           nFeatureTabLength;
    int           nFeatureTabIndex;
    int           nFeatureTabAlloc;
    bool          SetupParserExpat();
    GMLFeature   *NextFeatureExpat();
    char         *pabyBuf;
#endif

    VSILFILE*     fpGML;
    bool          m_bReadStarted;

    GMLReadState *m_poState;
    GMLReadState *m_poRecycledState;

    bool          m_bStopParsing;

    bool          SetupParser();
    void          CleanupParser();

    bool          m_bFetchAllGeometries;

    bool          m_bInvertAxisOrderIfLatLong;
    bool          m_bConsiderEPSGAsURN;
    GMLSwapCoordinatesEnum m_eSwapCoordinates;
    bool          m_bGetSecondaryGeometryOption;

    int           ParseFeatureType(CPLXMLNode *psSchemaNode,
                                const char* pszName,
                                const char *pszType);

    char         *m_pszGlobalSRSName;
    bool          m_bCanUseGlobalSRSName;

    char         *m_pszFilteredClassName;
    int           m_nFilteredClassIndex;

    int           m_nHasSequentialLayers;

    std::string   osElemPath;

    bool          m_bFaceHoleNegative;

    bool          m_bSetWidthFlag;

    bool          m_bReportAllAttributes;

    bool          m_bIsWFSJointLayer;

    bool          m_bEmptyAsNull;

    bool          ParseXMLHugeFile( const char *pszOutputFilename,
                                    const bool bSqliteIsTempFile,
                                    const int iSqliteCacheMB );

public:
                GMLReader(bool bExpatReader, bool bInvertAxisOrderIfLatLong,
                          bool bConsiderEPSGAsURN,
                          GMLSwapCoordinatesEnum eSwapCoordinates,
                          bool bGetSecondaryGeometryOption);
    virtual     ~GMLReader();

    bool             IsClassListLocked() const { return m_bClassListLocked; }
    void             SetClassListLocked( bool bFlag )
        { m_bClassListLocked = bFlag; }

    void             SetSourceFile( const char *pszFilename );
    void             SetFP( VSILFILE* fp );
    const char*      GetSourceFileName();

    int              GetClassCount() const { return m_nClassCount; }
    GMLFeatureClass *GetClass( int i ) const;
    GMLFeatureClass *GetClass( const char *pszName ) const;

    int              AddClass( GMLFeatureClass *poClass );
    void             ClearClasses();

    GMLFeature       *NextFeature();

    bool             LoadClasses( const char *pszFile = NULL );
    bool             SaveClasses( const char *pszFile = NULL );

    bool             ResolveXlinks( const char *pszFile,
                                    bool* pbOutIsTempFile,
                                    char **papszSkip = NULL,
                                    const bool bStrict = false );

    bool             HugeFileResolver( const char *pszFile,
                                       bool bSqliteIsTempFile,
                                       int iSqliteCacheMB );

    bool             PrescanForSchema(bool bGetExtents = true,
                                      bool bAnalyzeSRSPerFeature = true,
                                      bool bOnlyDetectSRS = false );
    bool             PrescanForTemplate( void );
    bool             ReArrangeTemplateClasses( GFSTemplateList *pCC );
    void             ResetReading();

// ---

    GMLReadState     *GetState() const { return m_poState; }
    void             PopState();
    void             PushState( GMLReadState * );

    bool             ShouldLookForClassAtAnyLevel() { return m_bLookForClassAtAnyLevel; }

    int         GetFeatureElementIndex( const char *pszElement, int nLen, GMLAppSchemaType eAppSchemaType );
    int         GetAttributeElementIndex( const char *pszElement, int nLen, const char* pszAttrKey = NULL );
    bool        IsCityGMLGenericAttributeElement( const char *pszElement, void* attr );

    void        PushFeature( const char *pszElement,
                             const char *pszFID,
                             int nClassIndex );

    void        SetFeaturePropertyDirectly( const char *pszElement,
                                            char *pszValue,
                                            int iPropertyIn,
                                            GMLPropertyType eType = GMLPT_Untyped );

    void        SetWidthFlag(bool bFlag) { m_bSetWidthFlag = bFlag; }

    bool        HasStoppedParsing() { return m_bStopParsing; }

    bool       FetchAllGeometries() { return m_bFetchAllGeometries; }

    void        SetGlobalSRSName( const char* pszGlobalSRSName ) ;
    const char* GetGlobalSRSName() { return m_pszGlobalSRSName; }

    bool        CanUseGlobalSRSName() { return m_bCanUseGlobalSRSName; }

    bool        SetFilteredClassName(const char* pszClassName);
    const char* GetFilteredClassName() { return m_pszFilteredClassName; }
    int         GetFilteredClassIndex() { return m_nFilteredClassIndex; }

    bool        IsSequentialLayers() const { return m_nHasSequentialLayers == TRUE; }

    void        SetReportAllAttributes(bool bFlag) { m_bReportAllAttributes = bFlag; }
    bool        ReportAllAttributes() const { return m_bReportAllAttributes; }

    void             SetIsWFSJointLayer( bool bFlag ) { m_bIsWFSJointLayer = bFlag; }
    bool             IsWFSJointLayer() const { return m_bIsWFSJointLayer; }

    void             SetEmptyAsNull( bool bFlag ) { m_bEmptyAsNull = bFlag; }
    bool             IsEmptyAsNull() const { return m_bEmptyAsNull; }

    static CPLMutex* hMutex;
};

#endif /* CPL_GMLREADERP_H_INCLUDED */
