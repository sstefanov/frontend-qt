/**
 * projectM-qt -- Qt4 based projectM GUI
 * Copyright (C)2003-2004 projectM Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * See 'LICENSE.txt' included within this release
 *
 */

#include <projectM-4/projectM.h>
#include <projectM-4/playlist.h>

#include <QIcon>
#include <QXmlStreamReader>
#include <QtDebug>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QSet>
#include <QRandomGenerator>
#include <QMessageBox>
#include <algorithm>
#include <vector>

#include "qplaylistmodel.hpp"
#include <QMimeData>

QString QPlaylistModel::PRESET_MIME_TYPE("text/x-projectM-preset");

QPlaylistModel::QPlaylistModel(projectm* _projectM, QObject * parent)
    : QAbstractTableModel(parent), m_projectM(_projectM), m_playlist(nullptr)
{
    // projectM 4.x: Create playlist manager
    m_playlist = projectm_playlist_create(m_projectM);
    if (!m_playlist) {
        qWarning() << "Failed to create playlist manager";
    } else {
        projectm_playlist_set_preset_switched_event_callback(m_playlist, &QPlaylistModel::presetSwitched, this);
        // Replaces the playlist's own handler, so automatic changes also go to the prepared preset
        projectm_set_preset_switch_requested_event_callback(m_projectM, &QPlaylistModel::switchRequested, this);
    }
}

void QPlaylistModel::presetSwitched(bool isHardCut, unsigned int index, void * userData)
{
    Q_UNUSED(isHardCut);
    QPlaylistModel * model = static_cast<QPlaylistModel *>(userData);
    char * filename = projectm_playlist_item(model->m_playlist, index);
    if (filename) {
        model->m_activePresetUrl = QString(filename);
        projectm_playlist_free_string(filename);
    }
    model->planNext();
}

void QPlaylistModel::switchRequested(bool isHardCut, void * userData)
{
    static_cast<QPlaylistModel *>(userData)->playNext(isHardCut);
}

void QPlaylistModel::planNext()
{
    const uint32_t size = projectm_playlist_size(m_playlist);
    if (size == 0)
        return;

    const uint32_t current = projectm_playlist_get_position(m_playlist);
    QVector<uint32_t> indices;
    if (projectm_playlist_get_shuffle(m_playlist) && size > 1) {
        // The library draws its random preset only when switching, too late to prepare it.
        // Several candidates are prepared, so the one that suits the music can be picked when switching.
        uint32_t wanted = SHUFFLE_CANDIDATES;
        if (wanted > size - 1)
            wanted = size - 1;
        while (static_cast<uint32_t>(indices.size()) < wanted) {
            const uint32_t index = QRandomGenerator::global()->bounded(size);
            if (index != current && !indices.contains(index))
                indices << index;
        }
    } else {
        indices << (current + 1) % size;
    }

    m_candidates.clear();
    QList<QByteArray> files;
    for (uint32_t index : indices) {
        char * filename = projectm_playlist_item(m_playlist, index);
        if (filename) {
            m_candidates.append({index, QByteArray(filename)});
            files << QByteArray(filename);
            projectm_playlist_free_string(filename);
        }
    }
    m_preloader.prepare(files);
}

void QPlaylistModel::playNext(bool hardCut)
{
    if (!projectm_playlist_get_shuffle(m_playlist)) {
        projectm_playlist_play_next(m_playlist, hardCut);
        return;
    }

    // Prepared candidates that are still at the same place in the list, calmest first
    QVector<QPair<float, uint32_t>> choices;
    const QList<QPresetPreloader::Prepared> prepared = m_preloader.ready();
    for (const QPresetPreloader::Prepared & preset : prepared) {
        for (const Candidate & candidate : m_candidates) {
            if (candidate.file != preset.file)
                continue;
            char * filename = projectm_playlist_item(m_playlist, candidate.index);
            if (!filename)
                continue;
            if (candidate.file == filename)
                choices.append({preset.motion, candidate.index});
            projectm_playlist_free_string(filename);
        }
    }

    if (choices.isEmpty()) {
        // Nothing is prepared yet, e.g. when switching again right away
        projectm_playlist_play_next(m_playlist, hardCut);
        return;
    }

    std::sort(choices.begin(), choices.end());
    // A picture that does not move at all with a beat playing is black or frozen
    while (choices.size() > 1 && choices.first().first < 0.0005f)
        choices.removeFirst();
    const float liveliness = m_musicLiveliness ? qBound(0.0f, m_musicLiveliness(), 1.0f) : 0.5f;
    const int choice = qRound(liveliness * (choices.size() - 1));
    projectm_playlist_set_position(m_playlist, choices[choice].second, hardCut);
}

QPlaylistModel::~QPlaylistModel()
{
    if (m_playlist) {
        projectm_set_preset_switch_requested_event_callback(m_projectM, nullptr, nullptr);
        projectm_playlist_destroy(m_playlist);
    }
}

int QPlaylistModel::rowCount(const QModelIndex & parent) const
{
    Q_UNUSED(parent);
    if (!m_playlist)
        return 0;
    return projectm_playlist_size(m_playlist);
}

int QPlaylistModel::columnCount(const QModelIndex & parent) const
{
    Q_UNUSED(parent);
    return 1;  // projectM 4.x: Only filename column
}

QVariant QPlaylistModel::data(const QModelIndex & index, int role) const
{
    if (!m_playlist || !index.isValid())
        return QVariant();

    int row = index.row();
    if (row < 0 || row >= static_cast<int>(projectm_playlist_size(m_playlist)))
        return QVariant();

    switch (role) {
        case Qt::DisplayRole:
        case URLInfoRole:
        case NameRole: {
            char* filename = projectm_playlist_item(m_playlist, row);
            if (filename) {
                QString path(filename);
                projectm_playlist_free_string(filename);

                if (role == URLInfoRole) {
                    // Return full path for URL info
                    return path;
                }
                // For DisplayRole and NameRole, return just the filename without .milk extension
                QString name = QFileInfo(path).fileName();
                if (name.endsWith(".milk", Qt::CaseInsensitive)) {
                    name.chop(5);  // Remove ".milk"
                }
                return name;
            }
            return QVariant();
        }

        case Qt::DecorationRole: {
            // Check if this is the currently playing preset
            uint32_t currentPos = projectm_playlist_get_position(m_playlist);
            if (row == static_cast<int>(currentPos)) {
                if (projectm_get_preset_locked(m_projectM)) {
                    return QIcon(":/icons/resources/icons/status_locked.png");
                }
                return QIcon(":/icons/resources/icons/status_playing.png");
            }
            return QVariant();
        }

        case Qt::ToolTipRole: {
            char* filename = projectm_playlist_item(m_playlist, row);
            if (filename) {
                QString tooltip(filename);
                projectm_playlist_free_string(filename);
                return tooltip;
            }
            return QVariant();
        }

        default:
            return QVariant();
    }
}

QVariant QPlaylistModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant();

    if (orientation == Qt::Horizontal) {
        return QString("Preset");
    }
    return QString::number(section + 1);
}

bool QPlaylistModel::setData(const QModelIndex & index, const QVariant & value, int role)
{
    Q_UNUSED(index);
    Q_UNUSED(value);
    Q_UNUSED(role);
    // projectM 4.x: Editing disabled (no ratings/names to edit)
    return false;
}

Qt::ItemFlags QPlaylistModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::ItemIsEnabled | Qt::ItemIsDropEnabled;

    return QAbstractItemModel::flags(index) | Qt::ItemIsDragEnabled;
}

void QPlaylistModel::appendRow(const QString & presetURL)
{
    if (!m_playlist)
        return;

    int oldSize = projectm_playlist_size(m_playlist);
    beginInsertRows(QModelIndex(), oldSize, oldSize);
    projectm_playlist_add_preset(m_playlist, presetURL.toLocal8Bit().data(), true);
    endInsertRows();
}

void QPlaylistModel::insertRow(int index, const QString & presetURL)
{
    if (!m_playlist)
        return;

    beginInsertRows(QModelIndex(), index, index);
    projectm_playlist_insert_preset(m_playlist, presetURL.toLocal8Bit().data(), index, true);
    endInsertRows();
}

bool QPlaylistModel::removeRow(int index, const QModelIndex & parent)
{
    Q_UNUSED(parent);
    if (!m_playlist)
        return false;

    if (index < 0 || index >= static_cast<int>(projectm_playlist_size(m_playlist)))
        return false;

    beginRemoveRows(QModelIndex(), index, index);
    bool result = projectm_playlist_remove_preset(m_playlist, index);
    endRemoveRows();
    return result;
}

bool QPlaylistModel::removeRows(int row, int count, const QModelIndex & parent)
{
    Q_UNUSED(parent);
    if (!m_playlist || count <= 0)
        return false;

    beginRemoveRows(QModelIndex(), row, row + count - 1);
    uint32_t removed = projectm_playlist_remove_presets(m_playlist, row, count);
    endRemoveRows();
    return removed > 0;
}

void QPlaylistModel::clearItems()
{
    if (!m_playlist)
        return;

    beginResetModel();
    projectm_playlist_clear(m_playlist);
    endResetModel();
}

void QPlaylistModel::clear()
{
    clearItems();
    m_playlistName.clear();
    m_playlistDesc.clear();
}

QString QPlaylistModel::userPresetPath()
{
    return QDir::homePath() + "/.local/share/projectM/presets";
}

bool QPlaylistModel::loadDefaultPresets(const QString & systemPresetDir)
{
    QStringList dirs, canonicalDirs;
    for (const QString & dir : {systemPresetDir, userPresetPath()}) {
        // Empty if the directory does not exist; also skips a user dir that links to the system one
        const QString canonical = QFileInfo(dir).canonicalFilePath();
        if (canonical.isEmpty() || !QFileInfo(canonical).isDir() || canonicalDirs.contains(canonical))
            continue;
        dirs << dir;
        canonicalDirs << canonical;
    }

    beginResetModel();
    projectm_playlist_clear(m_playlist);
    QStringList loaded;
    for (int i = 0; i < dirs.size(); ++i) {
        // A directory inside another one is already covered by the recursive scan
        bool nested = false;
        for (const QString & other : canonicalDirs)
            nested = nested || canonicalDirs[i].startsWith(other + '/');
        // The library's own duplicate check is quadratic: ~30 s for 140k presets
        if (!nested) {
            projectm_playlist_add_path(m_playlist, dirs[i].toLocal8Bit().data(), true, true);
            loaded << dirs[i];
        }
    }
    endResetModel();

    setTexturePaths(loaded);

    uint32_t size = projectm_playlist_size(m_playlist);
    if (size > 0) {
        projectm_playlist_set_position(m_playlist, QRandomGenerator::global()->bounded(size), true);
    }

    return size > 0;
}

// Presets find their textures by name, so projectM needs every directory below the preset roots that holds images
void QPlaylistModel::setTexturePaths(const QStringList & presetDirs)
{
    static const QStringList imageFilters{"*.jpg", "*.jpeg", "*.dds", "*.png", "*.tga", "*.bmp", "*.dib"};
    QSet<QString> found;
    for (const QString & dir : presetDirs) {
        QDirIterator it(dir, imageFilters, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            found.insert(it.fileInfo().absolutePath());
        }
    }

    // projectM scans each path recursively, so a directory below another listed one would be scanned twice
    QList<QByteArray> paths;
    for (const QString & dir : found) {
        bool nested = false;
        for (const QString & other : found)
            nested = nested || dir.startsWith(other + '/');
        if (!nested)
            paths << dir.toLocal8Bit();
    }

    std::vector<const char *> rawPaths;
    for (const QByteArray & path : paths)
        rawPaths.push_back(path.constData());
    projectm_set_texture_search_paths(m_projectM, rawPaths.data(), rawPaths.size());
}

bool QPlaylistModel::readPlaylist(const QString & file)
{
    QString defaultPresetPath = userPresetPath();

    // Determine the directory to load presets from
    QString presetDir;
    if (file.isEmpty()) {
        presetDir = defaultPresetPath;
    } else {
        QFileInfo fileInfo(file);
        if (fileInfo.isDir()) {
            presetDir = file;
        } else if (!fileInfo.exists()) {
            presetDir = defaultPresetPath;
        } else {
            // Regular file (XML playlist) - not supported in projectM 4.x
            qWarning() << "XML playlist files are not supported in projectM 4.x";
            return false;
        }
    }

    QFileInfo dirInfo(presetDir);
    if (!dirInfo.exists() || !dirInfo.isDir()) {
        qWarning() << "Preset directory does not exist:" << presetDir;
        return false;
    }

    return loadPresetsFromDirectory(presetDir);
}

bool QPlaylistModel::loadPresetsFromDirectory(const QString & path)
{
    beginResetModel();
    projectm_playlist_clear(m_playlist);
    // One recursive scan of a cleared playlist cannot produce duplicates; the library's check is quadratic
    projectm_playlist_add_path(m_playlist, path.toLocal8Bit().data(), true, true);
    endResetModel();

    setTexturePaths({path});

    uint32_t size = projectm_playlist_size(m_playlist);
    if (size > 0) {
        projectm_playlist_set_position(m_playlist, 0, true);
    }

    return size > 0;
}

bool QPlaylistModel::writePlaylist(const QString & file)
{
    Q_UNUSED(file);
    // projectM 4.x: XML writing not yet implemented
    qWarning() << "XML playlist writing not yet implemented for projectM 4.x";
    return false;
}

void QPlaylistModel::readPlaylistItem(QXmlStreamReader & reader)
{
    Q_UNUSED(reader);
    // projectM 4.x: Not implemented
}

bool QPlaylistModel::dropMimeData(const QMimeData *data, Qt::DropAction action,
                                  int row, int column, const QModelIndex &parent)
{
    Q_UNUSED(data);
    Q_UNUSED(action);
    Q_UNUSED(row);
    Q_UNUSED(column);
    Q_UNUSED(parent);
    // projectM 4.x: Drag and drop not yet implemented
    return false;
}

void QPlaylistModel::notifyDataChanged(unsigned int index)
{
    if (index < projectm_playlist_size(m_playlist)) {
        QModelIndex idx = this->index(index, 0);
        emit dataChanged(idx, idx);
    }
}

void QPlaylistModel::updateItemHighlights()
{
    // Refresh the entire model to update playing/locked icons
    int rows = rowCount();
    if (rows > 0) {
        emit dataChanged(index(0, 0), index(rows - 1, 0));
    }
}
