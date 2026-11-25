/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <memory>

#include <drc/live_drc.h>
#include <wx/listctrl.h>
#include <wx/panel.h>

class PCB_EDIT_FRAME;
class wxStaticText;


/**
 * Dockable list of the live DRC results.  Activating a row focuses the offending items.
 */
class PANEL_ONLINE_DRC : public wxPanel
{
public:
    PANEL_ONLINE_DRC( PCB_EDIT_FRAME* aFrame, LIVE_DRC* aLiveDRC );
    ~PANEL_ONLINE_DRC() override;

    /// Stop listening to the checker and the canvas.  Must be called before either goes away.
    void Shutdown();

private:
    void refresh();

    void onItemActivated( wxListEvent& aEvent );
    void onCanvasMotion( wxMouseEvent& aEvent );

private:
    PCB_EDIT_FRAME*                         m_frame;
    LIVE_DRC*                               m_liveDRC;
    std::shared_ptr<const LIVE_DRC_RESULTS> m_shown;   ///< Keeps listed markers alive
    wxStaticText*                           m_status;
    wxListCtrl*                             m_list;
    wxString                                m_canvasTooltip;
};
