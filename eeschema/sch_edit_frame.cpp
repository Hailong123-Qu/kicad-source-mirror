/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2017 Jean-Pierre Charras, jp.charras at wanadoo.fr
 * Copyright (C) 1992-2020 KiCad Developers, see AUTHORS.txt for contributors.
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

#include <base_units.h>
#include <class_library.h>
#include <confirm.h>
#include <connection_graph.h>
#include <dialog_symbol_remap.h>
#include <dialogs/dialog_schematic_find.h>
#include <eeschema_config.h>
#include <eeschema_id.h>
#include <executable_names.h>
#include <fctsys.h>
#include <general.h>
#include <gestfich.h>
#include <gr_basic.h>
#include <hierarch.h>
#include <html_messagebox.h>
#include <invoke_sch_dialog.h>
#include <kiface_i.h>
#include <kiway.h>
#include <lib_edit_frame.h>
#include <lib_view_frame.h>
#include <pgm_base.h>
#include <profile.h>
#include <project.h>
#include <reporter.h>
#include <sch_edit_frame.h>
#include <sch_painter.h>
#include <sch_sheet.h>
#include <schematic.h>
#include <advanced_config.h>
#include <sim/sim_plot_frame.h>
#include <symbol_lib_table.h>
#include <tool/action_toolbar.h>
#include <tool/common_control.h>
#include <tool/common_tools.h>
#include <tool/picker_tool.h>
#include <tool/tool_dispatcher.h>
#include <tool/tool_manager.h>
#include <tool/zoom_tool.h>
#include <tools/ee_actions.h>
#include <tools/ee_inspection_tool.h>
#include <tools/ee_point_editor.h>
#include <tools/ee_selection_tool.h>
#include <tools/sch_drawing_tools.h>
#include <tools/sch_edit_tool.h>
#include <tools/sch_editor_control.h>
#include <tools/sch_line_wire_bus_tool.h>
#include <tools/sch_move_tool.h>
#include <widgets/infobar.h>
#include <wildcards_and_files_ext.h>
#include <wx/cmdline.h>

#include <gal/graphics_abstraction_layer.h>

// non-member so it can be moved easily, and kept REALLY private.
// Do NOT Clear() in here.
static void add_search_paths( SEARCH_STACK* aDst, const SEARCH_STACK& aSrc, int aIndex )
{
    for( unsigned i=0; i<aSrc.GetCount();  ++i )
        aDst->AddPaths( aSrc[i], aIndex );
}


//-----<SCH "data on demand" functions>-------------------------------------------

SEARCH_STACK* PROJECT::SchSearchS()
{
    SEARCH_STACK* ss = (SEARCH_STACK*) GetElem( PROJECT::ELEM_SCH_SEARCH_STACK );

    wxASSERT( !ss || dynamic_cast<SEARCH_STACK*>( GetElem( PROJECT::ELEM_SCH_SEARCH_STACK ) ) );

    if( !ss )
    {
        ss = new SEARCH_STACK();

        // Make PROJECT the new SEARCH_STACK owner.
        SetElem( PROJECT::ELEM_SCH_SEARCH_STACK, ss );

        // to the empty SEARCH_STACK for SchSearchS(), add project dir as first
        ss->AddPaths( m_project_name.GetPath() );

        // next add the paths found in *.pro, variable "LibDir"
        wxString        libDir;

        try
        {
            PART_LIBS::LibNamesAndPaths( this, false, &libDir );
        }
        catch( const IO_ERROR& DBG( ioe ) )
        {
            DBG(printf( "%s: %s\n", __func__, TO_UTF8( ioe.What() ) );)
        }

        if( !!libDir )
        {
            wxArrayString   paths;

            SEARCH_STACK::Split( &paths, libDir );

            for( unsigned i =0; i<paths.GetCount();  ++i )
            {
                wxString path = AbsolutePath( paths[i] );

                ss->AddPaths( path );     // at the end
            }
        }

        // append all paths from aSList
        add_search_paths( ss, Kiface().KifaceSearch(), -1 );
    }

    return ss;
}


PART_LIBS* PROJECT::SchLibs()
{
    PART_LIBS* libs = (PART_LIBS*) GetElem( PROJECT::ELEM_SCH_PART_LIBS );

    wxASSERT( !libs || libs->Type() == PART_LIBS_T );

    if( !libs )
    {
        libs = new PART_LIBS();

        // Make PROJECT the new PART_LIBS owner.
        SetElem( PROJECT::ELEM_SCH_PART_LIBS, libs );

        try
        {
            libs->LoadAllLibraries( this );
        }
        catch( const PARSE_ERROR& pe )
        {
            wxString    lib_list = UTF8( pe.inputLine );
            wxWindow*   parent = Pgm().App().GetTopWindow();

            // parent of this dialog cannot be NULL since that breaks the Kiway() chain.
            HTML_MESSAGE_BOX dlg( parent, _( "Not Found" ) );

            dlg.MessageSet( _( "The following libraries were not found:" ) );

            dlg.ListSet( lib_list );

            dlg.Layout();

            dlg.ShowModal();
        }
        catch( const IO_ERROR& ioe )
        {
            wxWindow* parent = Pgm().App().GetTopWindow();

            DisplayError( parent, ioe.What() );
        }
    }

    return libs;
}

//-----</SCH "data on demand" functions>------------------------------------------


BEGIN_EVENT_TABLE( SCH_EDIT_FRAME, EDA_DRAW_FRAME )
    EVT_SOCKET( ID_EDA_SOCKET_EVENT_SERV, EDA_DRAW_FRAME::OnSockRequestServer )
    EVT_SOCKET( ID_EDA_SOCKET_EVENT, EDA_DRAW_FRAME::OnSockRequest )

    EVT_CLOSE( SCH_EDIT_FRAME::OnCloseWindow )
    EVT_SIZE( SCH_EDIT_FRAME::OnSize )

    EVT_MENU_RANGE( ID_FILE1, ID_FILEMAX, SCH_EDIT_FRAME::OnLoadFile )
    EVT_MENU( ID_FILE_LIST_CLEAR, SCH_EDIT_FRAME::OnClearFileHistory )

    EVT_MENU( ID_APPEND_PROJECT, SCH_EDIT_FRAME::OnAppendProject )
    EVT_MENU( ID_IMPORT_NON_KICAD_SCH, SCH_EDIT_FRAME::OnImportProject )

    EVT_MENU( wxID_EXIT, SCH_EDIT_FRAME::OnExit )
    EVT_MENU( wxID_CLOSE, SCH_EDIT_FRAME::OnExit )

    EVT_MENU( ID_GRID_SETTINGS, SCH_BASE_FRAME::OnGridSettings )
END_EVENT_TABLE()


SCH_EDIT_FRAME::SCH_EDIT_FRAME( KIWAY* aKiway, wxWindow* aParent ):
    SCH_BASE_FRAME( aKiway, aParent, FRAME_SCH, wxT( "Eeschema" ),
        wxDefaultPosition, wxDefaultSize, KICAD_DEFAULT_DRAWFRAME_STYLE, SCH_EDIT_FRAME_NAME ),
    m_highlightedConn( nullptr ),
    m_item_to_repeat( nullptr )
{
    m_schematic = new SCHEMATIC( &Prj() );

    m_defaults = &m_schematic->Settings();

    m_showBorderAndTitleBlock = true;   // true to show sheet references
    m_hasAutoSave = true;
    m_AboutTitle = "Eeschema";

    m_findReplaceDialog = nullptr;

    SetSpiceAdjustPassiveValues( false );

    // Give an icon
    wxIcon icon;
    icon.CopyFromBitmap( KiBitmap( icon_eeschema_xpm ) );
    SetIcon( icon );

    // Initialize grid id to the default value (50 mils):
    m_LastGridSizeId = ID_POPUP_GRID_LEVEL_50 - ID_POPUP_GRID_LEVEL_1000;

    LoadSettings( eeconfig() );

    CreateScreens();

    setupTools();
    ReCreateMenuBar();
    ReCreateHToolbar();
    ReCreateVToolbar();
    ReCreateOptToolbar();

    // Create the infobar
    m_infoBar = new WX_INFOBAR( this, &m_auimgr );

    // Initialize common print setup dialog settings.
    m_pageSetupData.GetPrintData().SetPrintMode( wxPRINT_MODE_PRINTER );
    m_pageSetupData.GetPrintData().SetQuality( wxPRINT_QUALITY_MEDIUM );
    m_pageSetupData.GetPrintData().SetBin( wxPRINTBIN_AUTO );
    m_pageSetupData.GetPrintData().SetNoCopies( 1 );

    m_auimgr.SetManagedWindow( this );

    m_auimgr.AddPane( m_mainToolBar,
                      EDA_PANE().HToolbar().Name( "MainToolbar" ).Top().Layer(6) );
    m_auimgr.AddPane( m_optionsToolBar,
                      EDA_PANE().VToolbar().Name( "OptToolbar" ).Left().Layer(3) );
    m_auimgr.AddPane( m_drawToolBar,
                      EDA_PANE().VToolbar().Name( "ToolsToolbar" ).Right().Layer(2) );
    m_auimgr.AddPane( m_infoBar,
                      EDA_PANE().InfoBar().Name( "InfoBar" ).Top().Layer(1) );
    m_auimgr.AddPane( GetCanvas(),
                      EDA_PANE().Canvas().Name( "DrawFrame" ).Center() );
    m_auimgr.AddPane( m_messagePanel,
                      EDA_PANE().Messages().Name( "MsgPanel" ).Bottom().Layer(6) );

    // Call Update() to fix all pane default sizes, especially the "InfoBar" pane before
    // hidding it.
    m_auimgr.Update();

    // We don't want the infobar displayed right away
    m_auimgr.GetPane( "InfoBar" ).Hide();
    m_auimgr.Update();

    GetToolManager()->RunAction( ACTIONS::zoomFitScreen, true );

    // Init grid size and visibility
    GetToolManager()->RunAction( ACTIONS::gridPreset, true, m_LastGridSizeId );

    if( GetCanvas() )
    {
        GetCanvas()->GetGAL()->SetGridVisibility( IsGridVisible() );
        GetCanvas()->GetGAL()->SetAxesEnabled( false );

        if( auto p = dynamic_cast<KIGFX::SCH_PAINTER*>( GetCanvas()->GetView()->GetPainter() ) )
            p->SetSchematic( m_schematic );
    }

    InitExitKey();

    // Net list generator
    DefaultExecFlags();

    UpdateTitle();

    // Default shutdown reason until a file is loaded
    SetShutdownBlockReason( _( "New schematic file is unsaved" ) );

    // Ensure the window is on top
    Raise();
}


SCH_EDIT_FRAME::~SCH_EDIT_FRAME()
{
    // Shutdown all running tools
    if( m_toolManager )
        m_toolManager->ShutdownAllTools();

    delete m_item_to_repeat;        // we own the cloned object, see this->SaveCopyForRepeatItem()

    SetScreen( NULL );

    delete m_schematic;
}


void SCH_EDIT_FRAME::setupTools()
{
    // Create the manager and dispatcher & route draw panel events to the dispatcher
    m_toolManager = new TOOL_MANAGER;
    m_toolManager->SetEnvironment( &Schematic(), GetCanvas()->GetView(),
                                   GetCanvas()->GetViewControls(), this );
    m_actions = new EE_ACTIONS();
    m_toolDispatcher = new TOOL_DISPATCHER( m_toolManager, m_actions );

    // Register tools
    m_toolManager->RegisterTool( new COMMON_CONTROL );
    m_toolManager->RegisterTool( new COMMON_TOOLS );
    m_toolManager->RegisterTool( new ZOOM_TOOL );
    m_toolManager->RegisterTool( new EE_SELECTION_TOOL );
    m_toolManager->RegisterTool( new PICKER_TOOL );
    m_toolManager->RegisterTool( new SCH_DRAWING_TOOLS );
    m_toolManager->RegisterTool( new SCH_LINE_WIRE_BUS_TOOL );
    m_toolManager->RegisterTool( new SCH_MOVE_TOOL );
    m_toolManager->RegisterTool( new SCH_EDIT_TOOL );
    m_toolManager->RegisterTool( new EE_INSPECTION_TOOL );
    m_toolManager->RegisterTool( new SCH_EDITOR_CONTROL );
    m_toolManager->RegisterTool( new EE_POINT_EDITOR );
    m_toolManager->InitTools();

    // Run the selection tool, it is supposed to be always active
    m_toolManager->RunAction( EE_ACTIONS::selectionActivate );

    GetCanvas()->SetEventDispatcher( m_toolDispatcher );
}


void SCH_EDIT_FRAME::SaveCopyForRepeatItem( SCH_ITEM* aItem )
{
    // we cannot store a pointer to an item in the display list here since
    // that item may be deleted, such as part of a line concatenation or other.
    // So simply always keep a copy of the object which is to be repeated.

    if( aItem )
    {
        delete m_item_to_repeat;

        m_item_to_repeat = (SCH_ITEM*) aItem->Clone();
        // Clone() preserves the flags, we want 'em cleared.
        m_item_to_repeat->ClearFlags();
    }
}


EDA_ITEM* SCH_EDIT_FRAME::GetItem( const KIID& aId )
{
    return Schematic().GetSheets().GetItem( aId );
}


void SCH_EDIT_FRAME::SetSheetNumberAndCount()
{
    SCH_SCREEN* screen;
    SCH_SCREENS s_list( Schematic().Root() );

    // Set the sheet count, and the sheet number (1 for root sheet)
    int              sheet_count       = Schematic().Root().CountSheets();
    int              sheet_number      = 1;
    const KIID_PATH& current_sheetpath = GetCurrentSheet().Path();

    // Examine all sheets path to find the current sheets path,
    // and count them from root to the current sheet path:
    for( const SCH_SHEET_PATH& sheet : Schematic().GetSheets() )
    {
        if( sheet.Path() == current_sheetpath )   // Current sheet path found
            break;

        sheet_number++;                         // Not found, increment before this current path
    }

    GetCurrentSheet().SetPageNumber( sheet_number );

    for( screen = s_list.GetFirst(); screen != NULL; screen = s_list.GetNext() )
        screen->m_NumberOfScreens = sheet_count;

    GetScreen()->m_ScreenNumber = sheet_number;
}


SCH_SCREEN* SCH_EDIT_FRAME::GetScreen() const
{
    return GetCurrentSheet().LastScreen();
}


SCHEMATIC& SCH_EDIT_FRAME::Schematic() const
{
    return *m_schematic;
}


wxString SCH_EDIT_FRAME::GetScreenDesc() const
{
    wxString s = GetCurrentSheet().PathHumanReadable();

    return s;
}


void SCH_EDIT_FRAME::CreateScreens()
{
    m_schematic->Reset();
    m_schematic->SetRoot( new SCH_SHEET( m_schematic ) );

    SCH_SCREEN* rootScreen = new SCH_SCREEN( m_schematic );
    rootScreen->SetMaxUndoItems( m_UndoRedoCountMax );
    m_schematic->Root().SetScreen( rootScreen );
    SetScreen( Schematic().RootScreen() );

    m_schematic->RootScreen()->SetFileName( wxEmptyString );

    GetCurrentSheet().push_back( &m_schematic->Root() );

    if( GetScreen() == NULL )
    {
        SCH_SCREEN* screen = new SCH_SCREEN( m_schematic );
        screen->SetMaxUndoItems( m_UndoRedoCountMax );
        SetScreen( screen );
    }

    GetScreen()->SetZoom( 32.0 );
}


SCH_SHEET_PATH& SCH_EDIT_FRAME::GetCurrentSheet() const
{
    return m_schematic->CurrentSheet();
}


void SCH_EDIT_FRAME::SetCurrentSheet( const SCH_SHEET_PATH& aSheet )
{
    if( aSheet != GetCurrentSheet() )
    {
        FocusOnItem( nullptr );

        Schematic().SetCurrentSheet( aSheet );
        GetCanvas()->DisplaySheet( aSheet.LastScreen() );
    }
}


void SCH_EDIT_FRAME::HardRedraw()
{
    FocusOnItem( nullptr );

    GetCanvas()->DisplaySheet( GetCurrentSheet().LastScreen() );
    GetCanvas()->ForceRefresh();
}


void SCH_EDIT_FRAME::OnCloseWindow( wxCloseEvent& aEvent )
{
    // Shutdown blocks must be determined and vetoed as early as possible
    if( SupportsShutdownBlockReason() && aEvent.GetId() == wxEVT_QUERY_END_SESSION
            && Schematic().GetSheets().IsModified() )
    {
        aEvent.Veto();
        return;
    }

    if( Kiface().IsSingle() )
    {
        LIB_EDIT_FRAME* libeditFrame = (LIB_EDIT_FRAME*) Kiway().Player( FRAME_SCH_LIB_EDITOR, false );
        if( libeditFrame && !libeditFrame->Close() )   // Can close component editor?
            return;

        LIB_VIEW_FRAME* viewlibFrame = (LIB_VIEW_FRAME*) Kiway().Player( FRAME_SCH_VIEWER, false );
        if( viewlibFrame && !viewlibFrame->Close() )   // Can close component viewer?
            return;

        viewlibFrame = (LIB_VIEW_FRAME*) Kiway().Player( FRAME_SCH_VIEWER_MODAL, false );

        if( viewlibFrame && !viewlibFrame->Close() )   // Can close modal component viewer?
            return;
    }

    SIM_PLOT_FRAME* simFrame = (SIM_PLOT_FRAME*) Kiway().Player( FRAME_SIMULATOR, false );

    if( simFrame && !simFrame->Close() )   // Can close the simulator?
        return;

    SCH_SHEET_LIST sheetlist = Schematic().GetSheets();

    if( sheetlist.IsModified() )
    {
        wxFileName fileName = Schematic().RootScreen()->GetFileName();
        wxString msg = _( "Save changes to \"%s\" before closing?" );

        if( !HandleUnsavedChanges( this, wxString::Format( msg, fileName.GetFullName() ),
                                   [&]()->bool { return SaveProject(); } ) )
        {
            aEvent.Veto();
            return;
        }
    }

    //
    // OK, we're really closing now.  No more returns after this.
    //

    // Shutdown all running tools ( and commit any pending change )
    if( m_toolManager )
        m_toolManager->ShutdownAllTools();

    // Close the find dialog and preserve it's setting if it is displayed.
    if( m_findReplaceDialog )
    {
        m_findStringHistoryList = m_findReplaceDialog->GetFindEntries();
        m_replaceStringHistoryList = m_findReplaceDialog->GetReplaceEntries();

        m_findReplaceDialog->Destroy();
        m_findReplaceDialog = nullptr;
    }

    if( FindHierarchyNavigator() )
        FindHierarchyNavigator()->Close( true );

    SCH_SCREENS screens( Schematic().Root() );
    wxFileName fn;

    for( SCH_SCREEN* screen = screens.GetFirst(); screen != NULL; screen = screens.GetNext() )
    {
        fn = Prj().AbsolutePath( screen->GetFileName() );

        // Auto save file name is the normal file name prepended with GetAutoSaveFilePrefix().
        fn.SetName( GetAutoSaveFilePrefix() + fn.GetName() );

        if( fn.FileExists() && fn.IsFileWritable() )
            wxRemoveFile( fn.GetFullPath() );
    }

    sheetlist.ClearModifyStatus();

    wxString fileName = Prj().AbsolutePath( Schematic().RootScreen()->GetFileName() );

    if( !Schematic().GetFileName().IsEmpty() && !Schematic().RootScreen()->IsEmpty() )
        UpdateFileHistory( fileName );

    Schematic().RootScreen()->Clear();

    // all sub sheets are deleted, only the main sheet is usable
    GetCurrentSheet().clear();

    Destroy();
}


wxString SCH_EDIT_FRAME::GetUniqueFilenameForCurrentSheet()
{
    // Filename is rootSheetName-sheetName-...-sheetName
    // Note that we need to fetch the rootSheetName out of its filename, as the root SCH_SHEET's
    // name is just a timestamp.

    wxFileName rootFn( GetCurrentSheet().at( 0 )->GetFileName() );
    wxString   filename = rootFn.GetName();

    for( unsigned i = 1; i < GetCurrentSheet().size(); i++ )
        filename += wxT( "-" ) + GetCurrentSheet().at( i )->GetName();

    return filename;
}


void SCH_EDIT_FRAME::OnModify()
{
    wxASSERT( GetScreen() );

    if( !GetScreen() )
        return;

    GetScreen()->SetModify();
    GetScreen()->SetSave();

    if( ADVANCED_CFG::GetCfg().m_realTimeConnectivity && CONNECTION_GRAPH::m_allowRealTime )
        RecalculateConnections( NO_CLEANUP );

    GetCanvas()->Refresh();
}


void SCH_EDIT_FRAME::OnUpdatePCB( wxCommandEvent& event )
{
    wxFileName fn = Prj().AbsolutePath( Schematic().GetFileName() );

    fn.SetExt( PcbFileExtension );

    if( Kiface().IsSingle() )
    {
        DisplayError( this,  _( "Cannot update the PCB, because the Schematic Editor is opened"
                                " in stand-alone mode. In order to create/update PCBs from"
                                " schematics, launch the Kicad shell and create a project." ) );
        return;
    }

    KIWAY_PLAYER* frame = Kiway().Player( FRAME_PCB_EDITOR, true );

    // a pcb frame can be already existing, but not yet used.
    // this is the case when running the footprint editor, or the footprint viewer first
    // if the frame is not visible, the board is not yet loaded
    if( !frame->IsVisible() )
    {
        frame->OpenProjectFiles( std::vector<wxString>( 1, fn.GetFullPath() ) );
        frame->Show( true );
    }

    // On Windows, Raise() does not bring the window on screen, when iconized
    if( frame->IsIconized() )
        frame->Iconize( false );

    frame->Raise();

    std::string payload;
    Kiway().ExpressMail( FRAME_PCB_EDITOR, MAIL_PCB_UPDATE, payload, this );
}


wxFindReplaceData* SCH_EDIT_FRAME::GetFindReplaceData()
{
    if( m_findReplaceDialog && m_findReplaceDialog->IsVisible()
            && !m_findReplaceData->GetFindString().IsEmpty() )
    {
        return m_findReplaceData;
    }

    return nullptr;
}


HIERARCHY_NAVIG_DLG* SCH_EDIT_FRAME::FindHierarchyNavigator()
{
    wxWindow* navigator = wxWindow::FindWindowByName( HIERARCHY_NAVIG_DLG_WNAME );

    return static_cast< HIERARCHY_NAVIG_DLG* >( navigator );
}

void SCH_EDIT_FRAME::UpdateHierarchyNavigator( bool aForceUpdate )
{
    if( aForceUpdate )
    {
        if( FindHierarchyNavigator() )
            FindHierarchyNavigator()->Close();

        HIERARCHY_NAVIG_DLG* hierarchyDialog = new HIERARCHY_NAVIG_DLG( this );

        hierarchyDialog->Show( true );
    }
    else
    {
        if( FindHierarchyNavigator() )
            FindHierarchyNavigator()->UpdateHierarchyTree();
    }
}


void SCH_EDIT_FRAME::ShowFindReplaceDialog( bool aReplace )
{
    if( m_findReplaceDialog )
        m_findReplaceDialog->Destroy();

    m_findReplaceDialog= new DIALOG_SCH_FIND( this, m_findReplaceData, wxDefaultPosition,
                                              wxDefaultSize, aReplace ? wxFR_REPLACEDIALOG : 0 );

    m_findReplaceDialog->SetFindEntries( m_findStringHistoryList );
    m_findReplaceDialog->SetReplaceEntries( m_replaceStringHistoryList );
    m_findReplaceDialog->Show( true );
}


void SCH_EDIT_FRAME::ShowFindReplaceStatus( const wxString& aMsg, int aStatusTime )
{
    // Prepare the infobar, since we don't know its state
    m_infoBar->RemoveAllButtons();
    m_infoBar->AddCloseButton();

    m_infoBar->ShowMessageFor( aMsg, aStatusTime, wxICON_INFORMATION );
}


void SCH_EDIT_FRAME::ClearFindReplaceStatus()
{
    m_infoBar->Dismiss();
}


void SCH_EDIT_FRAME::OnFindDialogClose()
{
    m_findStringHistoryList = m_findReplaceDialog->GetFindEntries();
    m_replaceStringHistoryList = m_findReplaceDialog->GetReplaceEntries();

    m_findReplaceDialog->Destroy();
    m_findReplaceDialog = nullptr;
}


void SCH_EDIT_FRAME::OnLoadFile( wxCommandEvent& event )
{
    wxString fn = GetFileFromHistory( event.GetId(), _( "Schematic" ) );

    if( fn.size() )
        OpenProjectFiles( std::vector<wxString>( 1, fn ) );
}


void SCH_EDIT_FRAME::OnClearFileHistory( wxCommandEvent& aEvent )
{
    ClearFileHistory();
}


void SCH_EDIT_FRAME::NewProject()
{
    wxString pro_dir = m_mruPath;

    wxFileDialog dlg( this, _( "New Schematic" ), pro_dir, wxEmptyString,
                      LegacySchematicFileWildcard(), wxFD_SAVE );

    if( dlg.ShowModal() != wxID_CANCEL )
    {
        // Enforce the extension, wxFileDialog is inept.
        wxFileName create_me = dlg.GetPath();
        create_me.SetExt( KiCadSchematicFileExtension );

        if( create_me.FileExists() )
        {
            wxString msg;
            msg.Printf( _( "Schematic file \"%s\" already exists." ), create_me.GetFullName() );
            DisplayError( this, msg );
            return ;
        }

        // OpenProjectFiles() requires absolute
        wxASSERT_MSG( create_me.IsAbsolute(), "wxFileDialog returned non-absolute path" );

        OpenProjectFiles( std::vector<wxString>( 1, create_me.GetFullPath() ), KICTL_CREATE );
        m_mruPath = create_me.GetPath();
    }
}


void SCH_EDIT_FRAME::LoadProject()
{
    wxString pro_dir = m_mruPath;
    wxString wildcards = KiCadSchematicFileWildcard();

    wildcards += "|" + LegacySchematicFileWildcard();

    wxFileDialog dlg( this, _( "Open Schematic" ), pro_dir, wxEmptyString,
                      wildcards, wxFD_OPEN | wxFD_FILE_MUST_EXIST );

    if( dlg.ShowModal() != wxID_CANCEL )
    {
        OpenProjectFiles( std::vector<wxString>( 1, dlg.GetPath() ) );
        m_mruPath = Prj().GetProjectPath();
    }
}


void SCH_EDIT_FRAME::OnOpenPcbnew( wxCommandEvent& event )
{
    wxFileName kicad_board = Prj().AbsolutePath( Schematic().GetFileName() );

    if( kicad_board.IsOk() )
    {
        kicad_board.SetExt( PcbFileExtension );
        wxFileName legacy_board( kicad_board );
        legacy_board.SetExt( LegacyPcbFileExtension );
        wxFileName& boardfn = legacy_board;

        if( !legacy_board.FileExists() || kicad_board.FileExists() )
            boardfn = kicad_board;

        if( Kiface().IsSingle() )
        {
            wxString filename = QuoteFullPath( boardfn );
            ExecuteFile( this, PCBNEW_EXE, filename );
        }
        else
        {
            KIWAY_PLAYER* frame = Kiway().Player( FRAME_PCB_EDITOR, true );

            // a pcb frame can be already existing, but not yet used.
            // this is the case when running the footprint editor, or the footprint viewer first
            // if the frame is not visible, the board is not yet loaded
             if( !frame->IsVisible() )
            {
                frame->OpenProjectFiles( std::vector<wxString>( 1, boardfn.GetFullPath() ) );
                frame->Show( true );
            }

            // On Windows, Raise() does not bring the window on screen, when iconized
            if( frame->IsIconized() )
                frame->Iconize( false );

            frame->Raise();
        }
    }
    else
    {
        ExecuteFile( this, PCBNEW_EXE );
    }
}


void SCH_EDIT_FRAME::OnOpenCvpcb( wxCommandEvent& event )
{
    wxFileName fn = Prj().AbsolutePath( Schematic().GetFileName() );
    fn.SetExt( NetlistFileExtension );

    if( !ReadyToNetlist() )
        return;

    try
    {
        KIWAY_PLAYER* player = Kiway().Player( FRAME_CVPCB, false );  // test open already.

        if( !player )
        {
            player = Kiway().Player( FRAME_CVPCB, true );
            player->Show( true );
        }

        sendNetlistToCvpcb();

        player->Raise();
    }
    catch( const IO_ERROR& )
    {
        DisplayError( this, _( "Could not open CvPcb" ) );
    }
}


void SCH_EDIT_FRAME::OnExit( wxCommandEvent& event )
{
    if( event.GetId() == wxID_EXIT )
        Kiway().OnKiCadExit();

    if( event.GetId() == wxID_CLOSE || Kiface().IsSingle() )
        Close( false );
}


void SCH_EDIT_FRAME::PrintPage( RENDER_SETTINGS* aSettings )
{
    wxString fileName = Prj().AbsolutePath( GetScreen()->GetFileName() );

    aSettings->GetPrintDC()->SetLogicalFunction( wxCOPY );
    GetScreen()->Print( aSettings );
    PrintWorkSheet( aSettings, GetScreen(), IU_PER_MILS, fileName );
}


bool SCH_EDIT_FRAME::isAutoSaveRequired() const
{
    // In case this event happens before g_RootSheet is initialized which does happen
    // on mingw64 builds.

    if( Schematic().IsValid() )
    {
        SCH_SCREENS screenList( Schematic().Root() );

        for( SCH_SCREEN* screen = screenList.GetFirst(); screen; screen = screenList.GetNext() )
        {
            if( screen->IsSave() )
                return true;
        }
    }

    return false;
}


void SCH_EDIT_FRAME::AddItemToScreenAndUndoList( SCH_ITEM* aItem, bool aUndoAppend )
{
    SCH_SCREEN* screen = GetScreen();

    wxCHECK_RET( aItem != NULL, wxT( "Cannot add null item to list." ) );

    SCH_SHEET*     parentSheet = nullptr;
    SCH_COMPONENT* parentComponent = nullptr;
    SCH_ITEM*      undoItem = aItem;

    if( aItem->Type() == SCH_SHEET_PIN_T )
    {
        parentSheet = (SCH_SHEET*) aItem->GetParent();

        wxCHECK_RET( parentSheet && parentSheet->Type() == SCH_SHEET_T,
                     wxT( "Cannot place sheet pin in invalid schematic sheet object." ) );

        undoItem = parentSheet;
    }

    else if( aItem->Type() == SCH_FIELD_T )
    {
        parentComponent = (SCH_COMPONENT*) aItem->GetParent();

        wxCHECK_RET( parentComponent && parentComponent->Type() == SCH_COMPONENT_T,
                     wxT( "Cannot place field in invalid schematic component object." ) );

        undoItem = parentComponent;
    }

    if( aItem->IsNew() )
    {
        if( aItem->Type() == SCH_SHEET_PIN_T )
        {
            // Sheet pins are owned by their parent sheet.
            SaveCopyInUndoList( undoItem, UR_CHANGED, aUndoAppend );     // save the parent sheet

            parentSheet->AddPin( (SCH_SHEET_PIN*) aItem );
        }
        else if( aItem->Type() == SCH_FIELD_T )
        {
            // Component fields are also owned by their parent, but new component fields
            // are handled elsewhere.
            wxLogMessage( wxT( "addCurrentItemToScreen: unexpected new SCH_FIELD" ) );
        }
        else
        {
            if( !screen->CheckIfOnDrawList( aItem ) )  // don't want a loop!
                AddToScreen( aItem );

            SaveCopyForRepeatItem( aItem );
            SaveCopyInUndoList( undoItem, UR_NEW, aUndoAppend );
        }

        // Update connectivity info for new item
        if( !aItem->IsMoving() )
            RecalculateConnections( LOCAL_CLEANUP );
    }

    aItem->ClearFlags( IS_NEW );

    screen->SetModify();
    RefreshItem( aItem );

    if( !aItem->IsMoving() && aItem->IsConnectable() )
    {
        std::vector< wxPoint > pts;
        aItem->GetConnectionPoints( pts );

        for( auto i = pts.begin(); i != pts.end(); i++ )
        {
            for( auto j = i + 1; j != pts.end(); j++ )
                TrimWire( *i, *j );

            if( screen->IsJunctionNeeded( *i, true ) )
                AddJunction( *i, true, false );
        }

        TestDanglingEnds();

        for( SCH_ITEM* item : aItem->ConnectedItems( GetCurrentSheet() ) )
            RefreshItem( item );
    }

    aItem->ClearEditFlags();
    GetCanvas()->Refresh();
}


void SCH_EDIT_FRAME::UpdateTitle()
{
    wxString title;

    if( GetScreen()->GetFileName().IsEmpty() )
    {
        title.Printf( _( "Eeschema" ) + wxT( " \u2014" ) + _( " [no file]" ) );
    }
    else
    {
        wxString    fileName = Prj().AbsolutePath( GetScreen()->GetFileName() );
        wxFileName  fn = fileName;

        title.Printf( _( "Eeschema" ) + wxT( " \u2014 %s [%s] \u2014 %s" ),
                      fn.GetFullName(),
                      GetCurrentSheet().PathHumanReadable(),
                      fn.GetPath() );

        if( fn.FileExists() )
        {
            if( !fn.IsFileWritable() )
                title += _( " [Read Only]" );
        }
        else
            title += _( " [no file]" );
    }

    SetTitle( title );
}


void SCH_EDIT_FRAME::RecalculateConnections( SCH_CLEANUP_FLAGS aCleanupFlags )
{
    SCH_SHEET_LIST list = Schematic().GetSheets();
    PROF_COUNTER   timer;

    // Ensure schematic graph is accurate
    if( aCleanupFlags == LOCAL_CLEANUP )
    {
        SchematicCleanUp( GetScreen() );
    }
    else if( aCleanupFlags == GLOBAL_CLEANUP )
    {
        for( const auto& sheet : list )
            SchematicCleanUp( sheet.LastScreen() );
    }

    timer.Stop();
    wxLogTrace( "CONN_PROFILE", "SchematicCleanUp() %0.4f ms", timer.msecs() );

    Schematic().ConnectionGraph()->Recalculate( list, true );
}


void SCH_EDIT_FRAME::CommonSettingsChanged( bool aEnvVarsChanged )
{
    SCH_BASE_FRAME::CommonSettingsChanged( aEnvVarsChanged );

    RecreateToolbars();
    Layout();
    SendSizeEvent();
}


void SCH_EDIT_FRAME::OnPageSettingsChange()
{
    // Rebuild the sheet view (draw area and any other items):
    DisplayCurrentSheet();
}


void SCH_EDIT_FRAME::ShowChangedLanguage()
{
    // call my base class
    SCH_BASE_FRAME::ShowChangedLanguage();

    // tooltips in toolbars
    RecreateToolbars();

    // status bar
    UpdateMsgPanel();

    // This ugly hack is to fix an option(left) toolbar update bug that seems to only affect
    // windows.  See https://bugs.launchpad.net/kicad/+bug/1816492.  For some reason, calling
    // wxWindow::Refresh() does not resolve the issue.  Only a resize event seems to force the
    // toolbar to update correctly.
#if defined( __WXMSW__ )
    PostSizeEvent();
#endif
}


void SCH_EDIT_FRAME::SetScreen( BASE_SCREEN* aScreen )
{
    SCH_BASE_FRAME::SetScreen( aScreen );
    GetCanvas()->DisplaySheet( static_cast<SCH_SCREEN*>( aScreen ) );
}


const BOX2I SCH_EDIT_FRAME::GetDocumentExtents() const
{
    int sizeX = GetScreen()->GetPageSettings().GetWidthIU();
    int sizeY = GetScreen()->GetPageSettings().GetHeightIU();

    return BOX2I( VECTOR2I(0, 0), VECTOR2I( sizeX, sizeY ) );
}

void SCH_EDIT_FRAME::FixupJunctions()
{
    // Save the current sheet, to retrieve it later
    auto currSheet = GetCurrentSheet();

    bool modified = false;

    SCH_SHEET_LIST sheetList = Schematic().GetSheets();

    for( const SCH_SHEET_PATH& sheet : sheetList )
    {
        // We require a set here to avoid adding multiple junctions to the same spot
        std::set<wxPoint> junctions;

        SetCurrentSheet( sheet );
        GetCurrentSheet().UpdateAllScreenReferences();

        auto screen = GetCurrentSheet().LastScreen();

        for( auto aItem : screen->Items().OfType( SCH_COMPONENT_T ) )
        {
            auto cmp = static_cast<SCH_COMPONENT*>( aItem );

            for( const SCH_PIN* pin : cmp->GetSchPins( &sheet ) )
            {
                auto pos = pin->GetPosition();

                // Test if a _new_ junction is needed, and add it if missing
                if( screen->IsJunctionNeeded( pos, true ) )
                    junctions.insert( pos );
            }
        }

        for( auto& pos : junctions )
            AddJunction( pos, false, false );

        if( junctions.size() )
            modified = true;
    }

    if( modified )
        OnModify();

    // Reselect the initial sheet:
    SetCurrentSheet( currSheet );
    GetCurrentSheet().UpdateAllScreenReferences();
    SetScreen( GetCurrentSheet().LastScreen() );
}


bool SCH_EDIT_FRAME::IsContentModified()
{
    return Schematic().GetSheets().IsModified();
}


bool SCH_EDIT_FRAME::GetShowAllPins() const
{
    EESCHEMA_SETTINGS* cfg = eeconfig();
    return cfg->m_Appearance.show_hidden_pins;
}


void SCH_EDIT_FRAME::FocusOnItem( SCH_ITEM* aItem )
{
    static KIID lastBrightenedItemID( niluuid );

    SCH_SHEET_LIST sheetList = Schematic().GetSheets();
    SCH_SHEET_PATH dummy;
    SCH_ITEM*      lastItem = sheetList.GetItem( lastBrightenedItemID, &dummy );

    if( lastItem && lastItem != aItem )
    {
        lastItem->ClearBrightened();

        RefreshItem( lastItem );
        lastBrightenedItemID = niluuid;
    }

    if( aItem )
    {
        aItem->SetBrightened();

        RefreshItem( aItem );
        lastBrightenedItemID = aItem->m_Uuid;

        FocusOnLocation( aItem->GetFocusPosition() );
    }
}


void SCH_EDIT_FRAME::ConvertTimeStampUuids()
{
    // Remove this once this method is fully implemented.  Otherwise, don't use it.
    wxCHECK( false, /* void */ );

    // Replace sheet and symbol time stamps with real UUIDs and update symbol instance
    // sheet paths using the new UUID based sheet paths.

    // Save the time stamp sheet paths.
    SCH_SHEET_LIST timeStampSheetPaths = Schematic().GetSheets();

    std::vector<KIID_PATH> oldSheetPaths = timeStampSheetPaths.GetPaths();

    // The root sheet now gets a permanent UUID.
    const_cast<KIID&>( Schematic().Root().m_Uuid ).ConvertTimestampToUuid();

    SCH_SCREENS schematic( Schematic().Root() );

    // Change the sheet and symbol time stamps to UUIDs.
    for( SCH_SCREEN* screen = schematic.GetFirst(); screen; screen = schematic.GetNext() )
    {
        for( auto sheet : screen->Items().OfType( SCH_SHEET_T ) )
            const_cast<KIID&>( sheet->m_Uuid ).ConvertTimestampToUuid();

        for( auto symbol : screen->Items().OfType( SCH_COMPONENT_T ) )
            const_cast<KIID&>( symbol->m_Uuid ).ConvertTimestampToUuid();
    }

    timeStampSheetPaths.ReplaceLegacySheetPaths( oldSheetPaths );
}


wxString SCH_EDIT_FRAME::GetCurrentFileName() const
{
    return Schematic().GetFileName();
}
