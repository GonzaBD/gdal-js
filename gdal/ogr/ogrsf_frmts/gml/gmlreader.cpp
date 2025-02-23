/******************************************************************************
 * $Id$
 *
 * Project:  GML Reader
 * Purpose:  Implementation of GMLReader class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2002, Frank Warmerdam
 * Copyright (c) 2008-2014, Even Rouault <even dot rouault at mines-paris dot org>
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

#include "gmlreaderp.h"
#include "gmlreader.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "gmlutils.h"
#include "cpl_conv.h"
#include <map>
#include "cpl_multiproc.h"

#define SUPPORT_GEOMETRY

#ifdef SUPPORT_GEOMETRY
#  include "ogr_geometry.h"
#endif

/************************************************************************/
/*                            ~IGMLReader()                             */
/************************************************************************/

IGMLReader::~IGMLReader()

{
}

/************************************************************************/
/* ==================================================================== */
/*                  No XERCES or EXPAT Library                          */
/* ==================================================================== */
/************************************************************************/
#if !defined(HAVE_XERCES) && !defined(HAVE_EXPAT)

/************************************************************************/
/*                          CreateGMLReader()                           */
/************************************************************************/

IGMLReader *CreateGMLReader(bool /*bUseExpatParserPreferably*/,
                            bool /*bInvertAxisOrderIfLatLong*/,
                            bool /*bConsiderEPSGAsURN*/,
                            GMLSwapCoordinatesEnum /* eSwapCoordinates */,
                            bool /*bGetSecondaryGeometryOption*/)
{
    CPLError( CE_Failure, CPLE_AppDefined,
              "Unable to create Xerces C++ or Expat based GML reader, Xerces or Expat support\n"
              "not configured into GDAL/OGR." );
    return NULL;
}

/************************************************************************/
/* ==================================================================== */
/*                  With XERCES or EXPAT Library                        */
/* ==================================================================== */
/************************************************************************/
#else /* defined(HAVE_XERCES) || defined(HAVE_EXPAT) */

/************************************************************************/
/*                          CreateGMLReader()                           */
/************************************************************************/

IGMLReader *CreateGMLReader(bool bUseExpatParserPreferably,
                            bool bInvertAxisOrderIfLatLong,
                            bool bConsiderEPSGAsURN,
                            GMLSwapCoordinatesEnum eSwapCoordinates,
                            bool bGetSecondaryGeometryOption)

{
    return new GMLReader(bUseExpatParserPreferably,
                         bInvertAxisOrderIfLatLong,
                         bConsiderEPSGAsURN,
                         eSwapCoordinates,
                         bGetSecondaryGeometryOption);
}

#endif

OGRGMLXercesState GMLReader::m_eXercesInitState = OGRGML_XERCES_UNINITIALIZED;
int GMLReader::m_nInstanceCount = 0;
CPLMutex *GMLReader::hMutex = NULL;

/************************************************************************/
/*                             GMLReader()                              */
/************************************************************************/

GMLReader::GMLReader(
#if !defined(HAVE_EXPAT) || !defined(HAVE_XERCES)
CPL_UNUSED
#endif
                     bool bUseExpatParserPreferably,
                     bool bInvertAxisOrderIfLatLong,
                     bool bConsiderEPSGAsURN,
                     GMLSwapCoordinatesEnum eSwapCoordinates,
                     bool bGetSecondaryGeometryOption)
{
#ifndef HAVE_XERCES
    bUseExpatReader = true;
#else
    bUseExpatReader = false;
#ifdef HAVE_EXPAT
    if(bUseExpatParserPreferably)
        bUseExpatReader = true;
#endif
#endif

#if defined(HAVE_EXPAT) && defined(HAVE_XERCES)
    if (bUseExpatReader)
        CPLDebug("GML", "Using Expat reader");
    else
        CPLDebug("GML", "Using Xerces reader");
#endif

    m_nClassCount = 0;
    m_papoClass = NULL;
    m_bLookForClassAtAnyLevel = false;

    m_bClassListLocked = false;

    m_poGMLHandler = NULL;
#ifdef HAVE_XERCES
    m_poSAXReader = NULL;
    m_poCompleteFeature = NULL;
    m_GMLInputSource = NULL;
    m_bEOF = false;
#endif
#ifdef HAVE_EXPAT
    oParser = NULL;
    ppoFeatureTab = NULL;
    nFeatureTabIndex = 0;
    nFeatureTabLength = 0;
    nFeatureTabAlloc = 0;
    pabyBuf = NULL;
#endif
    fpGML = NULL;
    m_bReadStarted = false;

    m_poState = NULL;
    m_poRecycledState = NULL;

    m_pszFilename = NULL;

    m_bStopParsing = false;

    /* A bit experimental. Not publicly advertized. See commented doc in drv_gml.html */
    m_bFetchAllGeometries = CPLTestBool(CPLGetConfigOption("GML_FETCH_ALL_GEOMETRIES", "NO"));

    m_bInvertAxisOrderIfLatLong = bInvertAxisOrderIfLatLong;
    m_bConsiderEPSGAsURN = bConsiderEPSGAsURN;
    m_eSwapCoordinates = eSwapCoordinates;
    m_bGetSecondaryGeometryOption = bGetSecondaryGeometryOption;

    m_pszGlobalSRSName = NULL;
    m_bCanUseGlobalSRSName = false;

    m_pszFilteredClassName = NULL;
    m_nFilteredClassIndex = -1;

    m_nHasSequentialLayers = -1;

    /* Must be in synced in OGR_G_CreateFromGML(), OGRGMLLayer::OGRGMLLayer() and GMLReader::GMLReader() */
    m_bFaceHoleNegative = CPLTestBool(CPLGetConfigOption("GML_FACE_HOLE_NEGATIVE", "NO"));

    m_bSetWidthFlag = true;

    m_bReportAllAttributes = false;

    m_bIsWFSJointLayer = false;
    m_bEmptyAsNull = true;
}

/************************************************************************/
/*                             ~GMLReader()                             */
/************************************************************************/

GMLReader::~GMLReader()

{
    ClearClasses();

    CPLFree( m_pszFilename );

    CleanupParser();

    delete m_poRecycledState;

#ifdef HAVE_XERCES
    {
    CPLMutexHolderD(&hMutex);
    --m_nInstanceCount;
    if( m_nInstanceCount == 0 && m_eXercesInitState == OGRGML_XERCES_INIT_SUCCESSFUL )
    {
        XMLPlatformUtils::Terminate();
        m_eXercesInitState = OGRGML_XERCES_UNINITIALIZED;
    }
    }
#endif
#ifdef HAVE_EXPAT
    CPLFree(pabyBuf);
#endif

    if (fpGML)
        VSIFCloseL(fpGML);
    fpGML = NULL;

    CPLFree(m_pszGlobalSRSName);

    CPLFree(m_pszFilteredClassName);
}

/************************************************************************/
/*                          SetSourceFile()                             */
/************************************************************************/

void GMLReader::SetSourceFile( const char *pszFilename )

{
    CPLFree( m_pszFilename );
    m_pszFilename = CPLStrdup( pszFilename );
}

/************************************************************************/
/*                       GetSourceFileName()                           */
/************************************************************************/

const char* GMLReader::GetSourceFileName()

{
    return m_pszFilename;
}

/************************************************************************/
/*                               SetFP()                                */
/************************************************************************/

void GMLReader::SetFP( VSILFILE* fp )
{
    fpGML = fp;
}

/************************************************************************/
/*                            SetupParser()                             */
/************************************************************************/

bool GMLReader::SetupParser()

{
    if (fpGML == NULL)
        fpGML = VSIFOpenL(m_pszFilename, "rt");
    if (fpGML != NULL)
        VSIFSeekL( fpGML, 0, SEEK_SET );

    int bRet = -1;
#ifdef HAVE_EXPAT
    if (bUseExpatReader)
        bRet = SetupParserExpat();
#endif

#ifdef HAVE_XERCES
    if (!bUseExpatReader)
        bRet = SetupParserXerces();
#endif
    if (bRet < 0)
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "SetupParser(): should not happen");
        return false;
    }

    if (!bRet)
        return false;

    m_bReadStarted = false;

    // Push an empty state.
    PushState( m_poRecycledState ? m_poRecycledState : new GMLReadState() );
    m_poRecycledState = NULL;

    return true;
}

#ifdef HAVE_XERCES
/************************************************************************/
/*                        SetupParserXerces()                           */
/************************************************************************/

bool GMLReader::SetupParserXerces()
{
    {
    CPLMutexHolderD(&hMutex);
    m_nInstanceCount++;
    if( m_eXercesInitState == OGRGML_XERCES_UNINITIALIZED )
    {
        try
        {
            XMLPlatformUtils::Initialize();
        }

        catch (const XMLException& toCatch)
        {
            CPLError( CE_Warning, CPLE_AppDefined,
                      "Exception initializing Xerces based GML reader.\n%s",
                      tr_strdup(toCatch.getMessage()) );
            m_eXercesInitState = OGRGML_XERCES_INIT_FAILED;
            return false;
        }
        m_eXercesInitState = OGRGML_XERCES_INIT_SUCCESSFUL;
    }
    if( m_eXercesInitState != OGRGML_XERCES_INIT_SUCCESSFUL )
        return false;
    }

    // Cleanup any old parser.
    if( m_poSAXReader != NULL )
        CleanupParser();

    // Create and initialize parser.
    XMLCh* xmlUriValid = NULL;
    XMLCh* xmlUriNS = NULL;

    try{
        m_poSAXReader = XMLReaderFactory::createXMLReader();

        GMLXercesHandler* poXercesHandler = new GMLXercesHandler( this );
        m_poGMLHandler = poXercesHandler;

        m_poSAXReader->setContentHandler( poXercesHandler );
        m_poSAXReader->setErrorHandler( poXercesHandler );
        m_poSAXReader->setLexicalHandler( poXercesHandler );
        m_poSAXReader->setEntityResolver( poXercesHandler );
        m_poSAXReader->setDTDHandler( poXercesHandler );

        xmlUriValid = XMLString::transcode("http://xml.org/sax/features/validation");
        xmlUriNS = XMLString::transcode("http://xml.org/sax/features/namespaces");

#if (OGR_GML_VALIDATION)
        m_poSAXReader->setFeature( xmlUriValid, true);
        m_poSAXReader->setFeature( xmlUriNS, true);

        m_poSAXReader->setFeature( XMLUni::fgSAX2CoreNameSpaces, true );
        m_poSAXReader->setFeature( XMLUni::fgXercesSchema, true );

//    m_poSAXReader->setDoSchema(true);
//    m_poSAXReader->setValidationSchemaFullChecking(true);
#else
        m_poSAXReader->setFeature( XMLUni::fgSAX2CoreValidation, false);

#if XERCES_VERSION_MAJOR >= 3
        m_poSAXReader->setFeature( XMLUni::fgXercesSchema, false);
#else
        m_poSAXReader->setFeature( XMLUni::fgSAX2CoreNameSpaces, false);
#endif

#endif
        XMLString::release( &xmlUriValid );
        XMLString::release( &xmlUriNS );
    }
    catch (...)
    {
        XMLString::release( &xmlUriValid );
        XMLString::release( &xmlUriNS );

        CPLError( CE_Warning, CPLE_AppDefined,
                  "Exception initializing Xerces based GML reader.\n" );
        return false;
    }

    if (m_GMLInputSource == NULL && fpGML != NULL)
        m_GMLInputSource = new GMLInputSource(fpGML);

    return true;
}
#endif

/************************************************************************/
/*                        SetupParserExpat()                            */
/************************************************************************/

#ifdef HAVE_EXPAT
bool GMLReader::SetupParserExpat()
{
    // Cleanup any old parser.
    if( oParser != NULL )
        CleanupParser();

    oParser = OGRCreateExpatXMLParser();
    m_poGMLHandler = new GMLExpatHandler( this, oParser );

    XML_SetElementHandler(oParser, GMLExpatHandler::startElementCbk, GMLExpatHandler::endElementCbk);
    XML_SetCharacterDataHandler(oParser, GMLExpatHandler::dataHandlerCbk);
    XML_SetUserData(oParser, m_poGMLHandler);

    if (pabyBuf == NULL)
        pabyBuf = (char*)VSI_MALLOC_VERBOSE(PARSER_BUF_SIZE);
    if (pabyBuf == NULL)
        return false;

    return true;
}
#endif

/************************************************************************/
/*                           CleanupParser()                            */
/************************************************************************/

void GMLReader::CleanupParser()

{
#ifdef HAVE_XERCES
    if( !bUseExpatReader && m_poSAXReader == NULL )
        return;
#endif

#ifdef HAVE_EXPAT
    if ( bUseExpatReader && oParser == NULL )
        return;
#endif

    while( m_poState )
        PopState();

#ifdef HAVE_XERCES
    delete m_poSAXReader;
    m_poSAXReader = NULL;
    delete m_GMLInputSource;
    m_GMLInputSource = NULL;
    delete m_poCompleteFeature;
    m_poCompleteFeature = NULL;
    m_bEOF = false;
#endif

#ifdef HAVE_EXPAT
    if (oParser)
        XML_ParserFree(oParser);
    oParser = NULL;

    for( int i=nFeatureTabIndex; i < nFeatureTabLength; i++ )
        delete ppoFeatureTab[i];
    CPLFree(ppoFeatureTab);
    nFeatureTabIndex = 0;
    nFeatureTabLength = 0;
    nFeatureTabAlloc = 0;
    ppoFeatureTab = NULL;

#endif

    delete m_poGMLHandler;
    m_poGMLHandler = NULL;

    m_bReadStarted = false;
}

#ifdef HAVE_XERCES

GMLBinInputStream::GMLBinInputStream(VSILFILE* fpIn)
{
    this->fp = fpIn;
    emptyString = 0;
}

GMLBinInputStream::~ GMLBinInputStream()
{
}

#if XERCES_VERSION_MAJOR >= 3
XMLFilePos GMLBinInputStream::curPos() const
{
    return (XMLFilePos)VSIFTellL(fp);
}

XMLSize_t GMLBinInputStream::readBytes(XMLByte* const toFill, const XMLSize_t maxToRead)
{
    return (XMLSize_t)VSIFReadL(toFill, 1, maxToRead, fp);
}

const XMLCh* GMLBinInputStream::getContentType() const
{
    return &emptyString;
}
#else
unsigned int GMLBinInputStream::curPos() const
{
    return (unsigned int)VSIFTellL(fp);
}

unsigned int GMLBinInputStream::readBytes(XMLByte* const toFill, const unsigned int maxToRead)
{
    return (unsigned int)VSIFReadL(toFill, 1, maxToRead, fp);
}
#endif

GMLInputSource::GMLInputSource(VSILFILE* fp, MemoryManager* const manager) : InputSource(manager)
{
    binInputStream = new GMLBinInputStream(fp);
}

GMLInputSource::~GMLInputSource()
{
}

BinInputStream* GMLInputSource::makeStream() const
{
    return binInputStream;
}

#endif // HAVE_XERCES

/************************************************************************/
/*                        NextFeatureXerces()                           */
/************************************************************************/

#ifdef HAVE_XERCES
GMLFeature *GMLReader::NextFeatureXerces()

{
    GMLFeature *poReturn = NULL;

    if (m_bEOF)
        return NULL;

    try
    {
        if( !m_bReadStarted )
        {
            if( m_poSAXReader == NULL )
                SetupParser();

            m_bReadStarted = true;

            if (m_poSAXReader == NULL || m_GMLInputSource == NULL)
                return NULL;

            if( !m_poSAXReader->parseFirst( *m_GMLInputSource, m_oToFill ) )
                return NULL;
        }

        while( m_poCompleteFeature == NULL
               && !m_bStopParsing
               && m_poSAXReader->parseNext( m_oToFill ) ) {}

        if (m_poCompleteFeature == NULL)
            m_bEOF = true;

        poReturn = m_poCompleteFeature;
        m_poCompleteFeature = NULL;

    }
    catch (const XMLException& toCatch)
    {
        char *pszErrorMessage = tr_strdup( toCatch.getMessage() );
        CPLDebug( "GML",
                  "Error during NextFeature()! Message:\n%s",
                  pszErrorMessage );
        CPLFree(pszErrorMessage);
        m_bStopParsing = true;
    }
    catch (const SAXException& toCatch)
    {
        char *pszErrorMessage = tr_strdup( toCatch.getMessage() );
        CPLError(CE_Failure, CPLE_AppDefined, "%s", pszErrorMessage);
        CPLFree(pszErrorMessage);
        m_bStopParsing = true;
    }

    return poReturn;
}
#endif

#ifdef HAVE_EXPAT
GMLFeature *GMLReader::NextFeatureExpat()

{
    if (!m_bReadStarted)
    {
        if (oParser == NULL)
            SetupParser();

        m_bReadStarted = true;
    }

    if (fpGML == NULL || m_bStopParsing)
        return NULL;

    if (nFeatureTabIndex < nFeatureTabLength)
    {
        return ppoFeatureTab[nFeatureTabIndex++];
    }

    if (VSIFEofL(fpGML))
        return NULL;

    nFeatureTabLength = 0;
    nFeatureTabIndex = 0;

    int nDone;
    do
    {
        /* Reset counter that is used to detect billion laugh attacks */
        ((GMLExpatHandler*)m_poGMLHandler)->ResetDataHandlerCounter();

        unsigned int nLen =
                (unsigned int)VSIFReadL( pabyBuf, 1, PARSER_BUF_SIZE, fpGML );
        nDone = VSIFEofL(fpGML);

        /* Some files, such as APT_AIXM.xml from https://nfdc.faa.gov/webContent/56DaySub/2015-03-05/aixm5.1.zip */
        /* end with trailing nul characters. This test is not fully bullet-proof in case */
        /* the nul characters would occur at a buffer boundary */
        while( nDone && nLen > 0 && pabyBuf[nLen-1] == '\0' )
            nLen --;

        if (XML_Parse(oParser, pabyBuf, nLen, nDone) == XML_STATUS_ERROR)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "XML parsing of GML file failed : %s "
                     "at line %d, column %d",
                     XML_ErrorString(XML_GetErrorCode(oParser)),
                     (int)XML_GetCurrentLineNumber(oParser),
                     (int)XML_GetCurrentColumnNumber(oParser));
            m_bStopParsing = true;
        }
        if (!m_bStopParsing)
            m_bStopParsing = ((GMLExpatHandler*)m_poGMLHandler)->HasStoppedParsing();

    } while (!nDone && !m_bStopParsing && nFeatureTabLength == 0);

    return (nFeatureTabLength) ? ppoFeatureTab[nFeatureTabIndex++] : NULL;
}
#endif

GMLFeature *GMLReader::NextFeature()
{
#ifdef HAVE_EXPAT
    if (bUseExpatReader)
        return NextFeatureExpat();
#endif

#ifdef HAVE_XERCES
    if (!bUseExpatReader)
        return NextFeatureXerces();
#endif

    CPLError(CE_Failure, CPLE_AppDefined, "NextFeature(): Should not happen");
    return NULL;
}

/************************************************************************/
/*                            PushFeature()                             */
/*                                                                      */
/*      Create a feature based on the named element.  If the            */
/*      corresponding feature class doesn't exist yet, then create      */
/*      it now.  A new GMLReadState will be created for the feature,    */
/*      and it will be placed within that state.  The state is          */
/*      pushed onto the readstate stack.                                */
/************************************************************************/

void GMLReader::PushFeature( const char *pszElement,
                             const char *pszFID,
                             int nClassIndex )

{
    int iClass;

    if( nClassIndex != INT_MAX )
    {
        iClass = nClassIndex;
    }
    else
    {
    /* -------------------------------------------------------------------- */
    /*      Find the class of this element.                                 */
    /* -------------------------------------------------------------------- */
        for( iClass = 0; iClass < m_nClassCount; iClass++ )
        {
            if( EQUAL(pszElement,m_papoClass[iClass]->GetElementName()) )
                break;
        }

    /* -------------------------------------------------------------------- */
    /*      Create a new feature class for this element, if there is no     */
    /*      existing class for it.                                          */
    /* -------------------------------------------------------------------- */
        if( iClass == m_nClassCount )
        {
            CPLAssert( !m_bClassListLocked );

            GMLFeatureClass *poNewClass = new GMLFeatureClass( pszElement );

            AddClass( poNewClass );
        }
    }

/* -------------------------------------------------------------------- */
/*      Create a feature of this feature class.  Try to set the fid     */
/*      if available.                                                   */
/* -------------------------------------------------------------------- */
    GMLFeature *poFeature = new GMLFeature( m_papoClass[iClass] );
    if( pszFID != NULL )
    {
        poFeature->SetFID( pszFID );
    }

/* -------------------------------------------------------------------- */
/*      Create and push a new read state.                               */
/* -------------------------------------------------------------------- */
    GMLReadState *poState;

    poState = m_poRecycledState ? m_poRecycledState : new GMLReadState();
    m_poRecycledState = NULL;
    poState->m_poFeature = poFeature;
    PushState( poState );
}

/************************************************************************/
/*                          IsFeatureElement()                          */
/*                                                                      */
/*      Based on context and the element name, is this element a new    */
/*      GML feature element?                                            */
/************************************************************************/

int GMLReader::GetFeatureElementIndex( const char *pszElement, int nElementLength,
                                       GMLAppSchemaType eAppSchemaType )

{
    const char *pszLast = m_poState->GetLastComponent();
    size_t      nLenLast = m_poState->GetLastComponentLen();

    if( eAppSchemaType == APPSCHEMA_MTKGML )
    {
        if( m_poState->m_nPathLength != 1 )
            return -1;
    }
    else if( (nLenLast >= 6 && EQUAL(pszLast+nLenLast-6,"member")) ||
        (nLenLast >= 7 && EQUAL(pszLast+nLenLast-7,"members")) )
    {
        /* Default feature name */
    }
    else
    {
        if (nLenLast == 4 && strcmp(pszLast, "dane") == 0)
        {
            /* Polish TBD GML */
        }

        /* Begin of OpenLS */
        else if (nLenLast == 19 && nElementLength == 15 &&
                 strcmp(pszLast, "GeocodeResponseList") == 0 &&
                 strcmp(pszElement, "GeocodedAddress") == 0)
        {
        }
        else if (nLenLast == 22 &&
                 strcmp(pszLast, "DetermineRouteResponse") == 0)
        {
            /* We don't want the children of RouteInstructionsList */
            /* to be a single feature. We want each RouteInstruction */
            /* to be a feature */
            if (strcmp(pszElement, "RouteInstructionsList") == 0)
                return -1;
        }
        else if (nElementLength == 16 && nLenLast == 21 &&
                 strcmp(pszElement, "RouteInstruction") == 0 &&
                 strcmp(pszLast, "RouteInstructionsList") == 0)
        {
        }
        /* End of OpenLS */

        else if (nLenLast > 6 && strcmp(pszLast + nLenLast - 6, "_layer") == 0 &&
                 nElementLength > 8 && strcmp(pszElement + nElementLength - 8, "_feature") == 0)
        {
            /* GML answer of MapServer WMS GetFeatureInfo request */
        }

        /* Begin of CSW SearchResults */
        else if (nElementLength == (int)strlen("BriefRecord") &&
                 nLenLast == (int)strlen("SearchResults") &&
                 strcmp(pszElement, "BriefRecord") == 0 &&
                 strcmp(pszLast, "SearchResults") == 0)
        {
        }
        else if (nElementLength == (int)strlen("SummaryRecord") &&
                 nLenLast == (int)strlen("SearchResults") &&
                 strcmp(pszElement, "SummaryRecord") == 0 &&
                 strcmp(pszLast, "SearchResults") == 0)
        {
        }
        else if (nElementLength == (int)strlen("Record") &&
                 nLenLast == (int)strlen("SearchResults") &&
                 strcmp(pszElement, "Record") == 0 &&
                 strcmp(pszLast, "SearchResults") == 0)
        {
        }
        /* End of CSW SearchResults */

        else
        {
            if( m_bClassListLocked )
            {
                for( int i = 0; i < m_nClassCount; i++ )
                {
                    if( m_poState->osPath.size() + 1 + nElementLength == m_papoClass[i]->GetElementNameLen() &&
                        m_papoClass[i]->GetElementName()[m_poState->osPath.size()] == '|' &&
                        memcmp(m_poState->osPath.c_str(), m_papoClass[i]->GetElementName(), m_poState->osPath.size()) == 0 &&
                        memcmp(pszElement,m_papoClass[i]->GetElementName() + 1 + m_poState->osPath.size(), nElementLength) == 0 )
                    {
                        return i;
                    }
                }
            }
            return -1;
        }
    }

    // If the class list isn't locked, any element that is a featureMember
    // will do.
    if( !m_bClassListLocked )
        return INT_MAX;

    // otherwise, find a class with the desired element name.
    for( int i = 0; i < m_nClassCount; i++ )
    {
        if( nElementLength == (int)m_papoClass[i]->GetElementNameLen() &&
            memcmp(pszElement,m_papoClass[i]->GetElementName(), nElementLength) == 0 )
            return i;
    }

    return -1;
}

/************************************************************************/
/*                IsCityGMLGenericAttributeElement()                    */
/************************************************************************/

bool GMLReader::IsCityGMLGenericAttributeElement( const char *pszElement, void* attr )

{
    if( strcmp(pszElement, "stringAttribute") != 0 &&
        strcmp(pszElement, "intAttribute") != 0 &&
        strcmp(pszElement, "doubleAttribute") != 0 )
        return false;

    char* pszVal = m_poGMLHandler->GetAttributeValue(attr, "name");
    if (pszVal == NULL)
        return false;

    GMLFeatureClass *poClass = m_poState->m_poFeature->GetClass();

    // If the schema is not yet locked, then any simple element
    // is potentially an attribute.
    if( !poClass->IsSchemaLocked() )
    {
        CPLFree(pszVal);
        return true;
    }

    for( int i = 0; i < poClass->GetPropertyCount(); i++ )
    {
        if( strcmp(poClass->GetProperty(i)->GetSrcElement(),pszVal) == 0 )
        {
            CPLFree(pszVal);
            return true;
        }
    }

    CPLFree(pszVal);
    return false;
}

/************************************************************************/
/*                       GetAttributeElementIndex()                     */
/************************************************************************/

int GMLReader::GetAttributeElementIndex( const char *pszElement, int nLen,
                                         const char *pszAttrKey )

{
    GMLFeatureClass *poClass = m_poState->m_poFeature->GetClass();

    // If the schema is not yet locked, then any simple element
    // is potentially an attribute.
    if( !poClass->IsSchemaLocked() )
        return INT_MAX;

    // Otherwise build the path to this element into a single string
    // and compare against known attributes.
    if( m_poState->m_nPathLength == 0 )
    {
        if( pszAttrKey == NULL )
            return poClass->GetPropertyIndexBySrcElement(pszElement, nLen);
        else
        {
            int nFullLen = nLen + 1 + static_cast<int>(strlen(pszAttrKey));
            osElemPath.reserve(nFullLen);
            osElemPath.assign(pszElement, nLen);
            osElemPath.append(1, '@');
            osElemPath.append(pszAttrKey);
            return poClass->GetPropertyIndexBySrcElement(osElemPath.c_str(), nFullLen);
        }
    }
    else
    {
        int nFullLen = nLen + static_cast<int>(m_poState->osPath.size()) + 1;
        if( pszAttrKey != NULL )
            nFullLen += 1 + static_cast<int>(strlen(pszAttrKey));
        osElemPath.reserve(nFullLen);
        osElemPath.assign(m_poState->osPath);
        osElemPath.append(1, '|');
        osElemPath.append(pszElement, nLen);
        if( pszAttrKey != NULL )
        {
            osElemPath.append(1, '@');
            osElemPath.append(pszAttrKey);
        }
        return poClass->GetPropertyIndexBySrcElement(osElemPath.c_str(), nFullLen);
    }
}

/************************************************************************/
/*                              PopState()                              */
/************************************************************************/

void GMLReader::PopState()

{
    if( m_poState != NULL )
    {
#ifdef HAVE_XERCES
        if( !bUseExpatReader && m_poState->m_poFeature != NULL &&
            m_poCompleteFeature == NULL )
        {
            m_poCompleteFeature = m_poState->m_poFeature;
            m_poState->m_poFeature = NULL;
        }
#endif

#ifdef HAVE_EXPAT
        if ( bUseExpatReader && m_poState->m_poFeature != NULL )
        {
            if (nFeatureTabLength >= nFeatureTabAlloc)
            {
                nFeatureTabAlloc = nFeatureTabLength * 4 / 3 + 16;
                ppoFeatureTab = (GMLFeature**)
                        CPLRealloc(ppoFeatureTab,
                                    sizeof(GMLFeature*) * (nFeatureTabAlloc));
            }
            ppoFeatureTab[nFeatureTabLength] = m_poState->m_poFeature;
            nFeatureTabLength++;

            m_poState->m_poFeature = NULL;
        }
#endif

        GMLReadState *poParent;

        poParent = m_poState->m_poParentState;

        delete m_poRecycledState;
        m_poRecycledState = m_poState;
        m_poRecycledState->Reset();
        m_poState = poParent;
    }
}

/************************************************************************/
/*                             PushState()                              */
/************************************************************************/

void GMLReader::PushState( GMLReadState *poState )

{
    poState->m_poParentState = m_poState;
    m_poState = poState;
}

/************************************************************************/
/*                              GetClass()                              */
/************************************************************************/

GMLFeatureClass *GMLReader::GetClass( int iClass ) const

{
    if( iClass < 0 || iClass >= m_nClassCount )
        return NULL;
    else
        return m_papoClass[iClass];
}

/************************************************************************/
/*                              GetClass()                              */
/************************************************************************/

GMLFeatureClass *GMLReader::GetClass( const char *pszName ) const

{
    for( int iClass = 0; iClass < m_nClassCount; iClass++ )
    {
        if( EQUAL(GetClass(iClass)->GetName(),pszName) )
            return GetClass(iClass);
    }

    return NULL;
}

/************************************************************************/
/*                              AddClass()                              */
/************************************************************************/

int GMLReader::AddClass( GMLFeatureClass *poNewClass )

{
    CPLAssert( GetClass( poNewClass->GetName() ) == NULL );

    m_nClassCount++;
    m_papoClass = (GMLFeatureClass **)
        CPLRealloc( m_papoClass, sizeof(void*) * m_nClassCount );
    m_papoClass[m_nClassCount-1] = poNewClass;

    if( poNewClass->HasFeatureProperties() )
        m_bLookForClassAtAnyLevel = true;

    return m_nClassCount-1;
}

/************************************************************************/
/*                            ClearClasses()                            */
/************************************************************************/

void GMLReader::ClearClasses()

{
    for( int i = 0; i < m_nClassCount; i++ )
        delete m_papoClass[i];
    CPLFree( m_papoClass );

    m_nClassCount = 0;
    m_papoClass = NULL;
    m_bLookForClassAtAnyLevel = false;
}

/************************************************************************/
/*                     SetFeaturePropertyDirectly()                     */
/*                                                                      */
/*      Set the property value on the current feature, adding the       */
/*      property name to the GMLFeatureClass if required.               */
/*      The pszValue ownership is passed to this function.              */
/************************************************************************/

void GMLReader::SetFeaturePropertyDirectly( const char *pszElement,
                                            char *pszValue,
                                            int iPropertyIn,
                                            GMLPropertyType eType )

{
    GMLFeature *poFeature = GetState()->m_poFeature;

    CPLAssert( poFeature  != NULL );

/* -------------------------------------------------------------------- */
/*      Does this property exist in the feature class?  If not, add     */
/*      it.                                                             */
/* -------------------------------------------------------------------- */
    GMLFeatureClass *poClass = poFeature->GetClass();
    int      iProperty;

    int nPropertyCount = poClass->GetPropertyCount();
    if (iPropertyIn >= 0 && iPropertyIn < nPropertyCount)
    {
        iProperty = iPropertyIn;
    }
    else
    {
        for( iProperty=0; iProperty < nPropertyCount; iProperty++ )
        {
            if( strcmp(poClass->GetProperty( iProperty )->GetSrcElement(),
                    pszElement ) == 0 )
                break;
        }

        if( iProperty == nPropertyCount )
        {
            if( poClass->IsSchemaLocked() )
            {
                CPLDebug("GML","Encountered property missing from class schema : %s.",
                         pszElement);
                CPLFree(pszValue);
                return;
            }

            CPLString osFieldName;

            if( IsWFSJointLayer() )
            {
                /* At that point the element path should be member|layer|property */

                /* Strip member| prefix. Should always be true normally */
                if( STARTS_WITH(pszElement, "member|") )
                    osFieldName = pszElement + strlen("member|");

                /* Replace layer|property by layer_property */
                size_t iPos = osFieldName.find('|');
                if( iPos != std::string::npos )
                    osFieldName[iPos] = '.';

                /* Special case for gml:id on layer */
                iPos = osFieldName.find("@id");
                if( iPos != std::string::npos )
                {
                    osFieldName.resize(iPos);
                    osFieldName += ".gml_id";
                }
            }
            else if( strchr(pszElement,'|') == NULL )
                osFieldName = pszElement;
            else
            {
                osFieldName = strrchr(pszElement,'|') + 1;
                if( poClass->GetPropertyIndex(osFieldName) != -1 )
                    osFieldName = pszElement;
            }

            size_t nPos = osFieldName.find("@");
            if( nPos != std::string::npos )
                osFieldName[nPos] = '_';

            // Does this conflict with an existing property name?
            while( poClass->GetProperty(osFieldName) != NULL )
            {
                osFieldName += "_";
            }

            GMLPropertyDefn *poPDefn = new GMLPropertyDefn(osFieldName,pszElement);

            if( EQUAL(CPLGetConfigOption( "GML_FIELDTYPES", ""), "ALWAYS_STRING") )
                poPDefn->SetType( GMLPT_String );
            else if( eType != GMLPT_Untyped )
                poPDefn->SetType( eType );

            if (poClass->AddProperty( poPDefn ) < 0)
            {
                delete poPDefn;
                CPLFree(pszValue);
                return;
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Set the property                                                */
/* -------------------------------------------------------------------- */
    poFeature->SetPropertyDirectly( iProperty, pszValue );

/* -------------------------------------------------------------------- */
/*      Do we need to update the property type?                         */
/* -------------------------------------------------------------------- */
    if( !poClass->IsSchemaLocked() )
    {
        poClass->GetProperty(iProperty)->AnalysePropertyValue(
                             poFeature->GetProperty(iProperty), m_bSetWidthFlag );
    }
}

/************************************************************************/
/*                            LoadClasses()                             */
/************************************************************************/

bool GMLReader::LoadClasses( const char *pszFile )

{
    // Add logic later to determine reasonable default schema file.
    if( pszFile == NULL )
        return false;

/* -------------------------------------------------------------------- */
/*      Load the raw XML file.                                          */
/* -------------------------------------------------------------------- */
    VSILFILE       *fp;
    int         nLength;
    char        *pszWholeText;

    fp = VSIFOpenL( pszFile, "rb" );

    if( fp == NULL )
    {
        CPLError( CE_Failure, CPLE_OpenFailed,
                  "Failed to open file %s.", pszFile );
        return false;
    }

    VSIFSeekL( fp, 0, SEEK_END );
    nLength = (int) VSIFTellL( fp );
    VSIFSeekL( fp, 0, SEEK_SET );

    pszWholeText = (char *) VSIMalloc(nLength+1);
    if( pszWholeText == NULL )
    {
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Failed to allocate %d byte buffer for %s,\n"
                  "is this really a GMLFeatureClassList file?",
                  nLength, pszFile );
        VSIFCloseL( fp );
        return false;
    }

    if( VSIFReadL( pszWholeText, nLength, 1, fp ) != 1 )
    {
        VSIFree( pszWholeText );
        VSIFCloseL( fp );
        CPLError( CE_Failure, CPLE_AppDefined,
                  "Read failed on %s.", pszFile );
        return false;
    }
    pszWholeText[nLength] = '\0';

    VSIFCloseL( fp );

    if( strstr( pszWholeText, "<GMLFeatureClassList" ) == NULL )
    {
        VSIFree( pszWholeText );
        CPLError( CE_Failure, CPLE_AppDefined,
                  "File %s does not contain a GMLFeatureClassList tree.",
                  pszFile );
        return false;
    }

/* -------------------------------------------------------------------- */
/*      Convert to XML parse tree.                                      */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psRoot;

    psRoot = CPLParseXMLString( pszWholeText );
    VSIFree( pszWholeText );

    // We assume parser will report errors via CPL.
    if( psRoot == NULL )
        return false;

    if( psRoot->eType != CXT_Element
        || !EQUAL(psRoot->pszValue,"GMLFeatureClassList") )
    {
        CPLDestroyXMLNode(psRoot);
        CPLError( CE_Failure, CPLE_AppDefined,
                  "File %s is not a GMLFeatureClassList document.",
                  pszFile );
        return false;
    }

    const char* pszSequentialLayers = CPLGetXMLValue(psRoot, "SequentialLayers", NULL);
    if (pszSequentialLayers)
        m_nHasSequentialLayers = CPLTestBool(pszSequentialLayers);

/* -------------------------------------------------------------------- */
/*      Extract feature classes for all definitions found.              */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psThis;

    for( psThis = psRoot->psChild; psThis != NULL; psThis = psThis->psNext )
    {
        if( psThis->eType == CXT_Element
            && EQUAL(psThis->pszValue,"GMLFeatureClass") )
        {
            GMLFeatureClass   *poClass;

            poClass = new GMLFeatureClass();

            if( !poClass->InitializeFromXML( psThis ) )
            {
                delete poClass;
                CPLDestroyXMLNode( psRoot );
                return false;
            }

            poClass->SetSchemaLocked( true );

            AddClass( poClass );
        }
    }

    CPLDestroyXMLNode( psRoot );

    SetClassListLocked( true );

    return true;
}

/************************************************************************/
/*                            SaveClasses()                             */
/************************************************************************/

bool GMLReader::SaveClasses( const char *pszFile )

{
    // Add logic later to determine reasonable default schema file.
    if( pszFile == NULL )
        return false;

/* -------------------------------------------------------------------- */
/*      Create in memory schema tree.                                   */
/* -------------------------------------------------------------------- */
    CPLXMLNode *psRoot;

    psRoot = CPLCreateXMLNode( NULL, CXT_Element, "GMLFeatureClassList" );

    if (m_nHasSequentialLayers != -1 && m_nClassCount > 1)
    {
        CPLCreateXMLElementAndValue( psRoot, "SequentialLayers",
                                     m_nHasSequentialLayers ? "true" : "false" );
    }

    for( int iClass = 0; iClass < m_nClassCount; iClass++ )
    {
        CPLAddXMLChild( psRoot, m_papoClass[iClass]->SerializeToXML() );
    }

/* -------------------------------------------------------------------- */
/*      Serialize to disk.                                              */
/* -------------------------------------------------------------------- */
    VSILFILE        *fp;
    bool         bSuccess = true;
    char        *pszWholeText = CPLSerializeXMLTree( psRoot );

    CPLDestroyXMLNode( psRoot );

    fp = VSIFOpenL( pszFile, "wb" );

    if( fp == NULL )
        bSuccess = false;
    else if( VSIFWriteL( pszWholeText, strlen(pszWholeText), 1, fp ) != 1 )
        bSuccess = false;
    else
        VSIFCloseL( fp );

    CPLFree( pszWholeText );

    return bSuccess;
}

/************************************************************************/
/*                          PrescanForSchema()                          */
/*                                                                      */
/*      For now we use a pretty dumb approach of just doing a normal    */
/*      scan of the whole file, building up the schema information.     */
/*      Eventually we hope to do a more efficient scan when just        */
/*      looking for schema information.                                 */
/************************************************************************/

bool GMLReader::PrescanForSchema( bool bGetExtents,
                                 bool bAnalyzeSRSPerFeature,
                                 bool bOnlyDetectSRS )

{
    GMLFeature  *poFeature;

    if( m_pszFilename == NULL )
        return false;

    if( !bOnlyDetectSRS )
    {
        SetClassListLocked( false );
        ClearClasses();
    }

    if( !SetupParser() )
        return false;

    m_bCanUseGlobalSRSName = true;

    GMLFeatureClass *poLastClass = NULL;

    m_nHasSequentialLayers = TRUE;

    void* hCacheSRS = GML_BuildOGRGeometryFromList_CreateCache();

    std::string osWork;

    while( (poFeature = NextFeature()) != NULL )
    {
        GMLFeatureClass *poClass = poFeature->GetClass();

        if (poLastClass != NULL && poClass != poLastClass &&
            poClass->GetFeatureCount() != -1)
            m_nHasSequentialLayers = false;
        poLastClass = poClass;

        if( poClass->GetFeatureCount() == -1 )
            poClass->SetFeatureCount( 1 );
        else
            poClass->SetFeatureCount( poClass->GetFeatureCount() + 1 );

        const CPLXMLNode* const * papsGeometry = poFeature->GetGeometryList();
        if( !bOnlyDetectSRS && papsGeometry != NULL && papsGeometry[0] != NULL )
        {
            if( poClass->GetGeometryPropertyCount() == 0 )
                poClass->AddGeometryProperty( new GMLGeometryPropertyDefn( "", "", wkbUnknown, -1, true ) );
        }

#ifdef SUPPORT_GEOMETRY
        if( bGetExtents && papsGeometry != NULL )
        {
            OGRGeometry *poGeometry = GML_BuildOGRGeometryFromList(
                papsGeometry, true, m_bInvertAxisOrderIfLatLong,
                NULL, m_bConsiderEPSGAsURN,
                m_eSwapCoordinates,
                m_bGetSecondaryGeometryOption,
                hCacheSRS, m_bFaceHoleNegative );

            if( poGeometry != NULL && poClass->GetGeometryPropertyCount() > 0 )
            {
                double  dfXMin, dfXMax, dfYMin, dfYMax;
                OGREnvelope sEnvelope;

                OGRwkbGeometryType eGType = (OGRwkbGeometryType)
                    poClass->GetGeometryProperty(0)->GetType();

                if( bAnalyzeSRSPerFeature )
                {
                    const char* pszSRSName = GML_ExtractSrsNameFromGeometry(papsGeometry,
                                                                            osWork,
                                                                            m_bConsiderEPSGAsURN);
                    if (pszSRSName != NULL)
                        m_bCanUseGlobalSRSName = false;
                    poClass->MergeSRSName(pszSRSName);
                }

                // Merge geometry type into layer.
                if( poClass->GetFeatureCount() == 1 && eGType == wkbUnknown )
                    eGType = wkbNone;

                poClass->GetGeometryProperty(0)->SetType(
                    (int) OGRMergeGeometryTypesEx(
                        eGType, poGeometry->getGeometryType(), true ) );

                // merge extents.
                if (!poGeometry->IsEmpty())
                {
                    poGeometry->getEnvelope( &sEnvelope );
                    if( poClass->GetExtents(&dfXMin, &dfXMax, &dfYMin, &dfYMax) )
                    {
                        dfXMin = MIN(dfXMin,sEnvelope.MinX);
                        dfXMax = MAX(dfXMax,sEnvelope.MaxX);
                        dfYMin = MIN(dfYMin,sEnvelope.MinY);
                        dfYMax = MAX(dfYMax,sEnvelope.MaxY);
                    }
                    else
                    {
                        dfXMin = sEnvelope.MinX;
                        dfXMax = sEnvelope.MaxX;
                        dfYMin = sEnvelope.MinY;
                        dfYMax = sEnvelope.MaxY;
                    }

                    poClass->SetExtents( dfXMin, dfXMax, dfYMin, dfYMax );
                }
                delete poGeometry;

            }
#endif /* def SUPPORT_GEOMETRY */
        }

        delete poFeature;
    }

    GML_BuildOGRGeometryFromList_DestroyCache(hCacheSRS);

    for( int i = 0; i < m_nClassCount; i++ )
    {
        GMLFeatureClass *poClass = m_papoClass[i];
        const char* pszSRSName = poClass->GetSRSName();

        if (m_bCanUseGlobalSRSName)
            pszSRSName = m_pszGlobalSRSName;

        OGRSpatialReference oSRS;
        if (m_bInvertAxisOrderIfLatLong && GML_IsSRSLatLongOrder(pszSRSName) &&
            oSRS.SetFromUserInput(pszSRSName) == OGRERR_NONE)
        {
            OGR_SRSNode *poGEOGCS = oSRS.GetAttrNode( "GEOGCS" );
            if( poGEOGCS != NULL )
                poGEOGCS->StripNodes( "AXIS" );

            OGR_SRSNode *poPROJCS = oSRS.GetAttrNode( "PROJCS" );
            if (poPROJCS != NULL && oSRS.EPSGTreatsAsNorthingEasting())
                poPROJCS->StripNodes( "AXIS" );

            char* pszWKT = NULL;
            if (oSRS.exportToWkt(&pszWKT) == OGRERR_NONE)
                poClass->SetSRSName(pszWKT);
            CPLFree(pszWKT);

            /* So when we have computed the extent, we didn't know yet */
            /* the SRS to use. Now we know it, we have to fix the extent */
            /* order */
            if (m_bCanUseGlobalSRSName)
            {
                double  dfXMin, dfXMax, dfYMin, dfYMax;
                if( poClass->GetExtents(&dfXMin, &dfXMax, &dfYMin, &dfYMax) )
                    poClass->SetExtents( dfYMin, dfYMax, dfXMin, dfXMax );
            }
        }
        else if( !bAnalyzeSRSPerFeature &&
                 pszSRSName != NULL &&
                 poClass->GetSRSName() == NULL &&
                 oSRS.SetFromUserInput(pszSRSName) == OGRERR_NONE )
        {
            char* pszWKT = NULL;
            if (oSRS.exportToWkt(&pszWKT) == OGRERR_NONE)
                poClass->SetSRSName(pszWKT);
            CPLFree(pszWKT);
        }
    }

    CleanupParser();

    return true;
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void GMLReader::ResetReading()

{
    CleanupParser();
    SetFilteredClassName(NULL);
}

/************************************************************************/
/*                          SetGlobalSRSName()                          */
/************************************************************************/

void GMLReader::SetGlobalSRSName( const char* pszGlobalSRSName )
{
    if (m_pszGlobalSRSName == NULL && pszGlobalSRSName != NULL)
    {
        const char* pszVertCS_EPSG;
        if( STARTS_WITH(pszGlobalSRSName, "EPSG:") &&
            (pszVertCS_EPSG = strstr(pszGlobalSRSName, ", EPSG:")) != NULL )
        {
            m_pszGlobalSRSName = CPLStrdup(CPLSPrintf("EPSG:%d+%d",
                    atoi(pszGlobalSRSName + 5),
                    atoi(pszVertCS_EPSG + 7)));
        }
        else if (STARTS_WITH(pszGlobalSRSName, "EPSG:") &&
            m_bConsiderEPSGAsURN)
        {
            m_pszGlobalSRSName = CPLStrdup(CPLSPrintf("urn:ogc:def:crs:EPSG::%s",
                                                      pszGlobalSRSName+5));
        }
        else
        {
            m_pszGlobalSRSName = CPLStrdup(pszGlobalSRSName);
        }
    }
}

/************************************************************************/
/*                       SetFilteredClassName()                         */
/************************************************************************/

bool GMLReader::SetFilteredClassName(const char* pszClassName)
{
    CPLFree(m_pszFilteredClassName);
    m_pszFilteredClassName = (pszClassName) ? CPLStrdup(pszClassName) : NULL;

    m_nFilteredClassIndex = -1;
    if( m_pszFilteredClassName != NULL )
    {
        for( int i = 0; i < m_nClassCount; i++ )
        {
            if( strcmp(m_papoClass[i]->GetElementName(), pszClassName) == 0 )
            {
                m_nFilteredClassIndex = i;
                break;
            }
        }
    }

    return true;
}
