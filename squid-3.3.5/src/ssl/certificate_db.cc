#include "squid.h"
#include "ssl/certificate_db.h"
#if HAVE_ERRNO_H
#include <errno.h>
#endif
#if HAVE_FSTREAM
#include <fstream>
#endif
#if HAVE_STDEXCEPT
#include <stdexcept>
#endif
#if HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#if HAVE_SYS_FILE_H
#include <sys/file.h>
#endif
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif

#define HERE "(ssl_crtd) " << __FILE__ << ':' << __LINE__ << ": "

Ssl::Lock::Lock(std::string const &aFilename) :
        filename(aFilename),
#if _SQUID_MSWIN_
        hFile(INVALID_HANDLE_VALUE)
#else
        fd(-1)
#endif
{
}

bool Ssl::Lock::locked() const
{
#if _SQUID_MSWIN_
    return hFile != INVALID_HANDLE_VALUE;
#else
    return fd != -1;
#endif
}

void Ssl::Lock::lock()
{

#if _SQUID_MSWIN_
    hFile = CreateFile(TEXT(filename.c_str()), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
#else
    fd = open(filename.c_str(), 0);
    if (fd == -1)
#endif
        throw std::runtime_error("Failed to open file " + filename);

#if _SQUID_MSWIN_
    if (!LockFile(hFile, 0, 0, 1, 0))
#else
    if (flock(fd, LOCK_EX) != 0)
#endif
        throw std::runtime_error("Failed to get a lock of " + filename);
}

void Ssl::Lock::unlock()
{
#if _SQUID_MSWIN_
    if (hFile != INVALID_HANDLE_VALUE) {
        UnlockFile(hFile, 0, 0, 1, 0);
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
    }
#else
    if (fd != -1) {
        flock(fd, LOCK_UN);
        close(fd);
        fd = -1;
    }
#endif
    else
        throw std::runtime_error("Lock is already unlocked for " + filename);
}

Ssl::Lock::~Lock()
{
    if (locked())
        unlock();
}

Ssl::Locker::Locker(Lock &aLock, const char *aFileName, int aLineNo):
        weLocked(false), lock(aLock), fileName(aFileName), lineNo(aLineNo)
{
    if (!lock.locked()) {
        lock.lock();
        weLocked = true;
    }
}

Ssl::Locker::~Locker()
{
    if (weLocked)
        lock.unlock();
}

Ssl::CertificateDb::Row::Row()
        :   width(cnlNumber)
{
    row = (char **)OPENSSL_malloc(sizeof(char *) * (width + 1));
    for (size_t i = 0; i < width + 1; ++i)
        row[i] = NULL;
}

Ssl::CertificateDb::Row::Row(char **aRow, size_t aWidth): width(aWidth)
{
    row = aRow;
}

Ssl::CertificateDb::Row::~Row()
{
    if (!row)
        return;

    void *max;
    if ((max = (void *)row[width]) != NULL) {
        // It is an openSSL allocated row. The TXT_DB_read function stores the
        // index and row items one one memory segment. The row[width] points
        // to the end of buffer. We have to check for items in the array which
        // are not stored in this segment. These items should released.
        for (size_t i = 0; i < width + 1; ++i) {
            if (((row[i] < (char *)row) || (row[i] > max)) && (row[i] != NULL))
                OPENSSL_free(row[i]);
        }
    } else {
        for (size_t i = 0; i < width + 1; ++i) {
            if (row[i])
                OPENSSL_free(row[i]);
        }
    }
    OPENSSL_free(row);
}

void Ssl::CertificateDb::Row::reset()
{
    row = NULL;
}

void Ssl::CertificateDb::Row::setValue(size_t cell, char const * value)
{
    assert(cell < width);
    if (row[cell]) {
        free(row[cell]);
    }
    if (value) {
        row[cell] = static_cast<char *>(OPENSSL_malloc(sizeof(char) * (strlen(value) + 1)));
        memcpy(row[cell], value, sizeof(char) * (strlen(value) + 1));
    } else
        row[cell] = NULL;
}

char ** Ssl::CertificateDb::Row::getRow()
{
    return row;
}

void Ssl::CertificateDb::sq_TXT_DB_delete(TXT_DB *db, const char **row)
{
    if (!db)
        return;

#if SQUID_SSLTXTDB_PSTRINGDATA
    for (int i = 0; i < sk_OPENSSL_PSTRING_num(db->data); ++i) {
        const char ** current_row = ((const char **)sk_OPENSSL_PSTRING_value(db->data, i));
#else
    for (int i = 0; i < sk_num(db->data); ++i) {
        const char ** current_row = ((const char **)sk_value(db->data, i));
#endif
        if (current_row == row) {
            sq_TXT_DB_delete_row(db, i);
            return;
        }
    }
}

#define countof(arr) (sizeof(arr)/sizeof(*arr))
void Ssl::CertificateDb::sq_TXT_DB_delete_row(TXT_DB *db, int idx)
{
    char **rrow;
#if SQUID_SSLTXTDB_PSTRINGDATA
    rrow = (char **)sk_OPENSSL_PSTRING_delete(db->data, idx);
#else
    rrow = (char **)sk_delete(db->data, idx);
#endif

    if (!rrow)
        return;

    Row row(rrow, cnlNumber); // row wrapper used to free the rrow

    const Columns db_indexes[]={cnlSerial, cnlName};
    for (unsigned int i = 0; i < countof(db_indexes); ++i) {
        void *data = NULL;
#if SQUID_SSLTXTDB_PSTRINGDATA
        if (LHASH_OF(OPENSSL_STRING) *fieldIndex =  db->index[db_indexes[i]])
            data = lh_OPENSSL_STRING_delete(fieldIndex, rrow);
#else
        if (LHASH *fieldIndex = db->index[db_indexes[i]])
            data = lh_delete(fieldIndex, rrow);
#endif
        if (data)
            assert(data == rrow);
    }
}

unsigned long Ssl::CertificateDb::index_serial_hash(const char **a)
{
    const char *n = a[Ssl::CertificateDb::cnlSerial];
    while (*n == '0')
        ++n;
    return lh_strhash(n);
}

int Ssl::CertificateDb::index_serial_cmp(const char **a, const char **b)
{
    const char *aa, *bb;
    for (aa = a[Ssl::CertificateDb::cnlSerial]; *aa == '0'; ++aa);
    for (bb = b[Ssl::CertificateDb::cnlSerial]; *bb == '0'; ++bb);
    return strcmp(aa, bb);
}

unsigned long Ssl::CertificateDb::index_name_hash(const char **a)
{
    return(lh_strhash(a[Ssl::CertificateDb::cnlName]));
}

int Ssl::CertificateDb::index_name_cmp(const char **a, const char **b)
{
    return(strcmp(a[Ssl::CertificateDb::cnlName], b[CertificateDb::cnlName]));
}

const std::string Ssl::CertificateDb::db_file("index.txt");
const std::string Ssl::CertificateDb::cert_dir("certs");
const std::string Ssl::CertificateDb::size_file("size");

Ssl::CertificateDb::CertificateDb(std::string const & aDb_path, size_t aMax_db_size, size_t aFs_block_size)
        :  db_path(aDb_path),
        db_full(aDb_path + "/" + db_file),
        cert_full(aDb_path + "/" + cert_dir),
        size_full(aDb_path + "/" + size_file),
        db(NULL),
        max_db_size(aMax_db_size),
        fs_block_size(aFs_block_size),
        dbLock(db_full),
        enabled_disk_store(true)
{
    if (db_path.empty() && !max_db_size)
        enabled_disk_store = false;
    else if ((db_path.empty() && max_db_size) || (!db_path.empty() && !max_db_size))
        throw std::runtime_error("ssl_crtd is missing the required parameter. There should be -s and -M parameters together.");
}

bool Ssl::CertificateDb::find(std::string const & host_name, Ssl::X509_Pointer & cert, Ssl::EVP_PKEY_Pointer & pkey)
{
    const Locker locker(dbLock, Here);
    load();
    return pure_find(host_name, cert, pkey);
}

bool Ssl::CertificateDb::purgeCert(std::string const & key)
{
    const Locker locker(dbLock, Here);
    load();
    if (!db)
        return false;

    if (!deleteByHostname(key))
        return false;

    save();
    return true;
}

bool Ssl::CertificateDb::addCertAndPrivateKey(Ssl::X509_Pointer & cert, Ssl::EVP_PKEY_Pointer & pkey, std::string const & useName)
{
    const Locker locker(dbLock, Here);
    load();
    if (!db || !cert || !pkey)
        return false;
    Row row;
    ASN1_INTEGER * ai = X509_get_serialNumber(cert.get());
    std::string serial_string;
    Ssl::BIGNUM_Pointer serial(ASN1_INTEGER_to_BN(ai, NULL));
    {
        TidyPointer<char, tidyFree> hex_bn(BN_bn2hex(serial.get()));
        serial_string = std::string(hex_bn.get());
    }
    row.setValue(cnlSerial, serial_string.c_str());
    char ** rrow = TXT_DB_get_by_index(db.get(), cnlSerial, row.getRow());
    // We are creating certificates with unique serial numbers. If the serial
    // number is found in the database, the same certificate is already stored.
    if (rrow != NULL) {
        // TODO: check if the stored row is valid.
        return true;
    }

    {
        TidyPointer<char, tidyFree> subject(X509_NAME_oneline(X509_get_subject_name(cert.get()), NULL, 0));
        Ssl::X509_Pointer findCert;
        Ssl::EVP_PKEY_Pointer findPkey;
        if (pure_find(useName.empty() ? subject.get() : useName, findCert, findPkey)) {
            // Replace with database certificate
            cert.reset(findCert.release());
            pkey.reset(findPkey.release());
            return true;
        }
        // pure_find may fail because the entry is expired, or because the
        // certs file is corrupted. Remove any entry with given hostname
        deleteByHostname(useName.empty() ? subject.get() : useName);
    }

    // check db size while trying to minimize calls to size()
    while (size() > max_db_size) {
        if (deleteInvalidCertificate())
            continue; // try to find another invalid certificate if needed

        // there are no more invalid ones, but there must be valid certificates
        do {
            if (!deleteOldestCertificate()) {
                save(); // Some entries may have been removed. Update the index file.
                return false; // errors prevented us from freeing enough space
            }
        } while (size() > max_db_size);
        break;
    }

    row.setValue(cnlType, "V");
    ASN1_UTCTIME * tm = X509_get_notAfter(cert.get());
    row.setValue(cnlExp_date, std::string(reinterpret_cast<char *>(tm->data), tm->length).c_str());
    row.setValue(cnlFile, "unknown");
    if (!useName.empty())
        row.setValue(cnlName, useName.c_str());
    else {
        TidyPointer<char, tidyFree> subject(X509_NAME_oneline(X509_get_subject_name(cert.get()), NULL, 0));
        row.setValue(cnlName, subject.get());
    }

    if (!TXT_DB_insert(db.get(), row.getRow())) {
        // failed to add index (???) but we may have already modified
        // the database so save before exit
        save();
        return false;
    }
    rrow = row.getRow();
    row.reset();

    std::string filename(cert_full + "/" + serial_string + ".pem");
    if (!writeCertAndPrivateKeyToFile(cert, pkey, filename.c_str())) {
        //remove row from txt_db and save
        sq_TXT_DB_delete(db.get(), (const char **)rrow);
        save();
        return false;
    }
    addSize(filename);

    save();
    return true;
}

void Ssl::CertificateDb::create(std::string const & db_path)
{
    if (db_path == "")
        throw std::runtime_error("Path to db is empty");
    std::string db_full(db_path + "/" + db_file);
    std::string cert_full(db_path + "/" + cert_dir);
    std::string size_full(db_path + "/" + size_file);

    if (mkdir(db_path.c_str(), 0777))
        throw std::runtime_error("Cannot create " + db_path);

    if (mkdir(cert_full.c_str(), 0777))
        throw std::runtime_error("Cannot create " + cert_full);

    std::ofstream size(size_full.c_str());
    if (size)
        size << 0;
    else
        throw std::runtime_error("Cannot open " + size_full + " to open");
    std::ofstream db(db_full.c_str());
    if (!db)
        throw std::runtime_error("Cannot open " + db_full + " to open");
}

void Ssl::CertificateDb::check(std::string const & db_path, size_t max_db_size)
{
    CertificateDb db(db_path, max_db_size, 0);
    db.load();
}

bool Ssl::CertificateDb::pure_find(std::string const & host_name, Ssl::X509_Pointer & cert, Ssl::EVP_PKEY_Pointer & pkey)
{
    if (!db)
        return false;

    Row row;
    row.setValue(cnlName, host_name.c_str());

    char **rrow = TXT_DB_get_by_index(db.get(), cnlName, row.getRow());
    if (rrow == NULL)
        return false;

    if (!sslDateIsInTheFuture(rrow[cnlExp_date]))
        return false;

    // read cert and pkey from file.
    std::string filename(cert_full + "/" + rrow[cnlSerial] + ".pem");
    readCertAndPrivateKeyFromFiles(cert, pkey, filename.c_str(), NULL);
    if (!cert || !pkey)
        return false;
    return true;
}

size_t Ssl::CertificateDb::size() const
{
    return readSize();
}

void Ssl::CertificateDb::addSize(std::string const & filename)
{
    writeSize(readSize() + getFileSize(filename));
}

void Ssl::CertificateDb::subSize(std::string const & filename)
{
    writeSize(readSize() - getFileSize(filename));
}

size_t Ssl::CertificateDb::readSize() const
{
    std::ifstream size_file(size_full.c_str());
    if (!size_file && enabled_disk_store)
        throw std::runtime_error("cannot open for reading: " + size_full);
    size_t db_size = 0;
    if (!(size_file >> db_size))
        throw std::runtime_error("error while reading " + size_full);
    return db_size;
}

void Ssl::CertificateDb::writeSize(size_t db_size)
{
    std::ofstream size_file(size_full.c_str());
    if (!size_file && enabled_disk_store)
        throw std::runtime_error("cannot write \"" + size_full + "\" file");
    size_file << db_size;
}

size_t Ssl::CertificateDb::getFileSize(std::string const & filename)
{
    std::ifstream file(filename.c_str(), std::ios::binary);
    file.seekg(0, std::ios_base::end);
    size_t file_size = file.tellg();
    return ((file_size + fs_block_size - 1) / fs_block_size) * fs_block_size;
}

void Ssl::CertificateDb::load()
{
    // Load db from file.
    Ssl::BIO_Pointer in(BIO_new(BIO_s_file()));
    if (!in || BIO_read_filename(in.get(), db_full.c_str()) <= 0)
        throw std::runtime_error("Uninitialized SSL certificate database directory: " + db_path + ". To initialize, run \"ssl_crtd -c -s " + db_path + "\".");

    bool corrupt = false;
    Ssl::TXT_DB_Pointer temp_db(TXT_DB_read(in.get(), cnlNumber));
    if (!temp_db)
        corrupt = true;

    // Create indexes in db.
    if (!corrupt && !TXT_DB_create_index(temp_db.get(), cnlSerial, NULL, LHASH_HASH_FN(index_serial_hash), LHASH_COMP_FN(index_serial_cmp)))
        corrupt = true;

    if (!corrupt && !TXT_DB_create_index(temp_db.get(), cnlName, NULL, LHASH_HASH_FN(index_name_hash), LHASH_COMP_FN(index_name_cmp)))
        corrupt = true;

    if (corrupt)
        throw std::runtime_error("The SSL certificate database " + db_path + " is corrupted. Please rebuild");

    db.reset(temp_db.release());
}

void Ssl::CertificateDb::save()
{
    if (!db)
        throw std::runtime_error("The certificates database is not loaded");;

    // To save the db to file,  create a new BIO with BIO file methods.
    Ssl::BIO_Pointer out(BIO_new(BIO_s_file()));
    if (!out || !BIO_write_filename(out.get(), const_cast<char *>(db_full.c_str())))
        throw std::runtime_error("Failed to initialize " + db_full + " file for writing");;

    if (TXT_DB_write(out.get(), db.get()) < 0)
        throw std::runtime_error("Failed to write " + db_full + " file");
}

// Normally defined in defines.h file
void Ssl::CertificateDb::deleteRow(const char **row, int rowIndex)
{
    const std::string filename(cert_full + "/" + row[cnlSerial] + ".pem");
    sq_TXT_DB_delete_row(db.get(), rowIndex);

    subSize(filename);
    int ret = remove(filename.c_str());
    if (ret < 0 && errno != ENOENT)
        throw std::runtime_error("Failed to remove certficate file " + filename + " from db");
}

bool Ssl::CertificateDb::deleteInvalidCertificate()
{
    if (!db)
        return false;

    bool removed_one = false;
#if SQUID_SSLTXTDB_PSTRINGDATA
    for (int i = 0; i < sk_OPENSSL_PSTRING_num(db.get()->data); ++i) {
        const char ** current_row = ((const char **)sk_OPENSSL_PSTRING_value(db.get()->data, i));
#else
    for (int i = 0; i < sk_num(db.get()->data); ++i) {
        const char ** current_row = ((const char **)sk_value(db.get()->data, i));
#endif

        if (!sslDateIsInTheFuture(current_row[cnlExp_date])) {
            deleteRow(current_row, i);
            removed_one = true;
            break;
        }
    }

    if (!removed_one)
        return false;
    return true;
}

bool Ssl::CertificateDb::deleteOldestCertificate()
{
    if (!db)
        return false;

#if SQUID_SSLTXTDB_PSTRINGDATA
    if (sk_OPENSSL_PSTRING_num(db.get()->data) == 0)
#else
    if (sk_num(db.get()->data) == 0)
#endif
        return false;

#if SQUID_SSLTXTDB_PSTRINGDATA
    const char **row = (const char **)sk_OPENSSL_PSTRING_value(db.get()->data, 0);
#else
    const char **row = (const char **)sk_value(db.get()->data, 0);
#endif

    deleteRow(row, 0);

    return true;
}

bool Ssl::CertificateDb::deleteByHostname(std::string const & host)
{
    if (!db)
        return false;

#if SQUID_SSLTXTDB_PSTRINGDATA
    for (int i = 0; i < sk_OPENSSL_PSTRING_num(db.get()->data); ++i) {
        const char ** current_row = ((const char **)sk_OPENSSL_PSTRING_value(db.get()->data, i));
#else
    for (int i = 0; i < sk_num(db.get()->data); ++i) {
        const char ** current_row = ((const char **)sk_value(db.get()->data, i));
#endif
        if (host == current_row[cnlName]) {
            deleteRow(current_row, i);
            return true;
        }
    }
    return false;
}

bool Ssl::CertificateDb::IsEnabledDiskStore() const
{
    return enabled_disk_store;
}
