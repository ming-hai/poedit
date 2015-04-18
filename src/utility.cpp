/*
 *  This file is part of Poedit (http://poedit.net)
 *
 *  Copyright (C) 2010-2015 Vaclav Slavik
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
 *
 */

#include "utility.h"

#include <stdio.h>
#include <wx/filename.h>
#include <wx/log.h>
#include <wx/config.h>
#include <wx/display.h>

#ifdef __WXOSX__
#include "osx_helpers.h"
#endif

wxString EscapeMarkup(const wxString& str)
{
    wxString s(str);
    s.Replace("&", "&amp;");
    s.Replace("<", "&lt;");
    s.Replace(">", "&gt;");
    return s;
}

// ----------------------------------------------------------------------
// TempDirectory
// ----------------------------------------------------------------------

bool TempDirectory::ms_keepFiles = false;

TempDirectory::TempDirectory() : m_counter(0)
{
#ifdef HAVE_MKDTEMP
    wxString path = wxFileName::GetTempDir();
    path += "/poeditXXXXXX";
    wxCharBuffer buf(path.fn_str());
    if ( mkdtemp(buf.data()) == NULL )
    {
        wxLogError(_("Cannot create temporary directory."));
        return;
    }
    m_dir = wxConvFile.cMB2WX(buf.data());
#else
    for ( ;; )
    {
        wxString name = wxFileName::CreateTempFileName("poedit");
        if ( name.empty() )
        {
            wxLogError(_("Cannot create temporary directory."));
            return;
        }

        wxLogNull null;
        if ( wxRemoveFile(name) && wxMkdir(name, 0700) )
        {
#ifdef __WXMSW__
            // prevent possible problems with Unicode filenames in launched
            // third party tools (e.g. gettext):
            m_dir = wxFileName(name).GetShortPath();
#else
            m_dir = name;
#endif
            wxLogTrace("poedit.tmp", "created temp dir %s", name.c_str());
            break;
        }
        // else: try again
    }
#endif
}


TempDirectory::~TempDirectory()
{
    if ( m_dir.empty() )
        return;

    if ( ms_keepFiles )
    {
        wxLogTrace("poedit.tmp", "keeping temp files in %s", m_dir.c_str());
        return;
    }

    for ( wxArrayString::const_iterator i = m_files.begin(); i != m_files.end(); ++i )
    {
        if ( wxFileName::FileExists(*i) )
        {
            wxLogTrace("poedit.tmp", "removing temp file %s", i->c_str());
            wxRemoveFile(*i);
        }
    }

    wxLogTrace("poedit.tmp", "removing temp dir %s", m_dir.c_str());
    wxFileName::Rmdir(m_dir);
}


wxString TempDirectory::CreateFileName(const wxString& suffix)
{
    wxString s = wxString::Format("%s%c%d%s",
                                  m_dir.c_str(), wxFILE_SEP_PATH,
                                  m_counter++,
                                  suffix.c_str());
    m_files.push_back(s);
    wxLogTrace("poedit.tmp", "new temp file %s", s.c_str());
    return s;
}


// ----------------------------------------------------------------------
// TempOutputFile
// ----------------------------------------------------------------------

TempOutputFileFor::TempOutputFileFor(const wxString& filename)
{
    wxString path, name, ext;
    wxFileName::SplitPath(filename, &path, &name, &ext);
    if (!ext.empty())
        ext = "." + ext;

#ifdef __WXOSX__
    NSURL *fileUrl = [NSURL fileURLWithPath:wxStringToNS(filename)];
    NSURL *tempdirUrl =
        [[NSFileManager defaultManager] URLForDirectory:NSItemReplacementDirectory
                                               inDomain:NSUserDomainMask
                                      appropriateForURL:[fileUrl URLByDeletingLastPathComponent]
                                                 create:YES
                                                  error:nil];
    if (tempdirUrl)
        m_tempDir = wxStringFromNS([tempdirUrl path]);
#endif

    wxString counter;
    for (;;)
    {
#ifdef __WXOSX__
        if (!m_tempDir.empty())
        {
            m_filenameTmp = m_tempDir + wxFILE_SEP_PATH + name + counter + ext;
        }
        else
#endif // __WXOSX__
        {
            // Temp filenames may be ugly, nobody cares. Make them safe for
            // Unicode-unfriendly uses on Windows, i.e. 8.3 without non-ASCII
            // characters:
            auto base = CliSafeFileName(path) + wxFILE_SEP_PATH;
#ifdef __WXMSW__
            // this is OK, ToAscii() replaces non-ASCII with '_':
            base += name.ToAscii();
#else
            base += name;
#endif
            m_filenameTmp = base + ".temp" + counter + ext;
        }

        if (!wxFileExists(m_filenameTmp))
            break; // good!

        counter += wchar_t('a' + rand() % 26);
    }
}


TempOutputFileFor::~TempOutputFileFor()
{
#ifdef __WXOSX__
    if (!m_tempDir.empty())
        wxFileName::Rmdir(m_tempDir, wxPATH_RMDIR_RECURSIVE);
#endif
}

#ifdef __WXMSW__
wxString CliSafeFileName(const wxString& fn)
{
    if (fn.IsAscii())
    {
        return fn;
    }
    else if (wxFileExists(fn) || wxDirExists(fn))
    {
        return wxFileName(fn).GetShortPath();
    }
    else
    {
        wxString path, name, ext;
        wxFileName::SplitPath(fn, &path, &name, &ext);
        if (wxDirExists(path))
        {
            auto p = wxFileName(path).GetShortPath() + wxFILE_SEP_PATH + name;
            if (!ext.empty())
                p += "." + ext;
            return p;
        }
    }
    return fn;
}
#endif // __WXMSW__


// ----------------------------------------------------------------------
// Helpers for persisting windows' state
// ----------------------------------------------------------------------

void SaveWindowState(const wxTopLevelWindow *win, int flags)
{
#ifdef __WXOSX__
    // Don't remember dimensions of a fullscreen window:
    if ([[NSApplication sharedApplication] currentSystemPresentationOptions] & NSApplicationPresentationFullScreen)
        return;
#endif

    wxConfigBase *cfg = wxConfig::Get();
    const wxString path = WindowStatePath(win);

    if ( !win->IsIconized() )
    {
        if ( !win->IsMaximized() )
        {
#if defined(__WXMSW__) || defined(__WXOSX__)

            if ( flags & WinState_Pos )
            {
                const wxPoint pos = win->GetPosition();
                cfg->Write(path + "x", (long)pos.x);
                cfg->Write(path + "y", (long)pos.y);
            }
#endif // __WXMSW__/__WXOSX__
            if ( flags &  WinState_Size )
            {
                const wxSize sz = win->GetClientSize();
                cfg->Write(path + "w", (long)sz.x);
                cfg->Write(path + "h", (long)sz.y);
            }
        }

        if ( flags &  WinState_Size )
            cfg->Write(path + "maximized", (long)win->IsMaximized());
    }
}


void RestoreWindowState(wxTopLevelWindow *win, const wxSize& defaultSize, int flags)
{
    wxConfigBase *cfg = wxConfig::Get();
    const wxString path = WindowStatePath(win);

    if ( flags & WinState_Size )
    {
        int width = (int)cfg->Read(path + "w", defaultSize.x);
        int height = (int)cfg->Read(path + "h", defaultSize.y);
        if ( width != -1 || height != -1 )
        {
            // filter out ridiculous sizes:
            if (width != -1 && width < 100)
                width = defaultSize.x;
            if (height != -1 && height < 100)
                height = defaultSize.y;
            win->SetClientSize(width, height);
        }
    }

#if defined(__WXMSW__) || defined(__WXOSX__)
    if ( flags & WinState_Pos )
    {
        wxPoint pos;
        pos.x = (int)cfg->Read(path + "x", -1);
        pos.y = (int)cfg->Read(path + "y", -1);
        if ( pos.x != -1 || pos.y != -1 )
        {
            // NB: if this is the only Poedit frame opened, place it at remembered
            //     position, but don't do that if there already are other frames,
            //     because they would overlap and nobody could recognize that there are
            //     many of them
            for (;;)
            {
                bool occupied = false;
                for (auto& w : wxTopLevelWindows)
                {
                    if (w != win && w->GetPosition() == pos)
                    {
                        occupied = true;
                        break;
                    }
                }
                if (!occupied)
                    break;
                pos += wxPoint(20,20);
            }

            win->Move(pos);
        }
    }

    // If the window is completely out of all screens (e.g. because
    // screens configuration changed), move it to primary screen:
    if ( wxDisplay::GetFromWindow(win) == wxNOT_FOUND )
        win->Move(20, 40);
#endif // __WXMSW__/__WXOSX__

    // If the window is larger than current screen, resize it to fit:
    int display = wxDisplay::GetFromWindow(win);
    if ( display == wxNOT_FOUND )
        return;

    wxRect screenRect = wxDisplay(display).GetClientArea();

    wxRect winRect = win->GetRect();
    if ( winRect.GetPosition() == wxDefaultPosition )
        winRect.SetPosition(screenRect.GetPosition()); // not placed yet, fake it

    if ( !screenRect.Contains(winRect) )
    {
        // Don't crop the window immediately, because it could become too small
        // due to it. Try to move it to the center of the screen first, then crop.
        winRect = winRect.CenterIn(screenRect);
        winRect.Intersect(screenRect);
        win->SetSize(winRect);
    }

    // Maximize if it should be
    if ( cfg->Read(path + "maximized", long(0)) )
    {
        win->Maximize();
    }
}
